//===- SIFixSGPRCopies.cpp - Remove potential VGPR => SGPR copies ---------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
/// \file
/// Copies from VGPR to SGPR registers are illegal and the register coalescer
/// will sometimes generate these illegal copies in situations like this:
///
///  Register Class <vsrc> is the union of <vgpr> and <sgpr>
///
/// BB0:
///   %0 <sgpr> = SCALAR_INST
///   %1 <vsrc> = COPY %0 <sgpr>
///    ...
///    BRANCH %cond BB1, BB2
///  BB1:
///    %2 <vgpr> = VECTOR_INST
///    %3 <vsrc> = COPY %2 <vgpr>
///  BB2:
///    %4 <vsrc> = PHI %1 <vsrc>, <%bb.0>, %3 <vrsc>, <%bb.1>
///    %5 <vgpr> = VECTOR_INST %4 <vsrc>
///
///
/// The coalescer will begin at BB0 and eliminate its copy, then the resulting
/// code will look like this:
///
/// BB0:
///   %0 <sgpr> = SCALAR_INST
///    ...
///    BRANCH %cond BB1, BB2
/// BB1:
///   %2 <vgpr> = VECTOR_INST
///   %3 <vsrc> = COPY %2 <vgpr>
/// BB2:
///   %4 <sgpr> = PHI %0 <sgpr>, <%bb.0>, %3 <vsrc>, <%bb.1>
///   %5 <vgpr> = VECTOR_INST %4 <sgpr>
///
/// Now that the result of the PHI instruction is an SGPR, the register
/// allocator is now forced to constrain the register class of %3 to
/// <sgpr> so we end up with final code like this:
///
/// BB0:
///   %0 <sgpr> = SCALAR_INST
///    ...
///    BRANCH %cond BB1, BB2
/// BB1:
///   %2 <vgpr> = VECTOR_INST
///   %3 <sgpr> = COPY %2 <vgpr>
/// BB2:
///   %4 <sgpr> = PHI %0 <sgpr>, <%bb.0>, %3 <sgpr>, <%bb.1>
///   %5 <vgpr> = VECTOR_INST %4 <sgpr>
///
/// Now this code contains an illegal copy from a VGPR to an SGPR.
///
/// In order to avoid this problem, this pass searches for PHI instructions
/// which define a <vsrc> register and constrains its definition class to
/// <vgpr> if the user of the PHI's definition register is a vector instruction.
/// If the PHI's definition class is constrained to <vgpr> then the coalescer
/// will be unable to perform the COPY removal from the above example  which
/// ultimately led to the creation of an illegal COPY.
//===----------------------------------------------------------------------===//

#include "AMDGPU.h"
#include "AMDGPUSubtarget.h"
#include "SIInstrInfo.h"
#include "SIRegisterInfo.h"
#include "MCTargetDesc/AMDGPUMCTargetDesc.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/CodeGen/MachineBasicBlock.h"
#include "llvm/CodeGen/MachineDominators.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/CodeGen/MachineInstr.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"
#include "llvm/CodeGen/MachineOperand.h"
#include "llvm/CodeGen/MachineRegisterInfo.h"
#include "llvm/CodeGen/TargetRegisterInfo.h"
#include "llvm/Pass.h"
#include "llvm/Support/CodeGen.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Target/TargetMachine.h"
#include <cassert>
#include <cstdint>
#include <iterator>
#include <list>
#include <map>
#include <tuple>
#include <utility>

using namespace llvm;

#define DEBUG_TYPE "si-fix-sgpr-copies"

static cl::opt<bool> EnableM0Merge(
  "amdgpu-enable-merge-m0",
  cl::desc("Merge and hoist M0 initializations"),
  cl::init(true));

namespace {

class SIFixSGPRCopies : public MachineFunctionPass {
  MachineDominatorTree *MDT;

public:
  static char ID;

  MachineRegisterInfo *MRI;
  const SIRegisterInfo *TRI;
  const SIInstrInfo *TII;

  SIFixSGPRCopies() : MachineFunctionPass(ID) {}

  bool runOnMachineFunction(MachineFunction &MF) override;

  void processPHINode(MachineInstr &MI);

  StringRef getPassName() const override { return "SI Fix SGPR copies"; }

  void getAnalysisUsage(AnalysisUsage &AU) const override {
    AU.addRequired<MachineDominatorTree>();
    AU.addPreserved<MachineDominatorTree>();
    AU.setPreservesCFG();
    MachineFunctionPass::getAnalysisUsage(AU);
  }
};

} // end anonymous namespace

INITIALIZE_PASS_BEGIN(SIFixSGPRCopies, DEBUG_TYPE,
                     "SI Fix SGPR copies", false, false)
INITIALIZE_PASS_DEPENDENCY(MachineDominatorTree)
INITIALIZE_PASS_END(SIFixSGPRCopies, DEBUG_TYPE,
                     "SI Fix SGPR copies", false, false)

char SIFixSGPRCopies::ID = 0;

char &llvm::SIFixSGPRCopiesID = SIFixSGPRCopies::ID;

FunctionPass *llvm::createSIFixSGPRCopiesPass() {
  return new SIFixSGPRCopies();
}

static bool hasVectorOperands(const MachineInstr &MI,
                              const SIRegisterInfo *TRI) {
  const MachineRegisterInfo &MRI = MI.getParent()->getParent()->getRegInfo();
  for (unsigned i = 0, e = MI.getNumOperands(); i != e; ++i) {
    if (!MI.getOperand(i).isReg() ||
        !Register::isVirtualRegister(MI.getOperand(i).getReg()))
      continue;

    if (TRI->hasVectorRegisters(MRI.getRegClass(MI.getOperand(i).getReg())))
      return true;
  }
  return false;
}

static std::pair<const TargetRegisterClass *, const TargetRegisterClass *>
getCopyRegClasses(const MachineInstr &Copy,
                  const SIRegisterInfo &TRI,
                  const MachineRegisterInfo &MRI) {
  Register DstReg = Copy.getOperand(0).getReg();
  Register SrcReg = Copy.getOperand(1).getReg();

  const TargetRegisterClass *SrcRC = Register::isVirtualRegister(SrcReg)
                                         ? MRI.getRegClass(SrcReg)
                                         : TRI.getPhysRegClass(SrcReg);

  // We don't really care about the subregister here.
  // SrcRC = TRI.getSubRegClass(SrcRC, Copy.getOperand(1).getSubReg());

  const TargetRegisterClass *DstRC = Register::isVirtualRegister(DstReg)
                                         ? MRI.getRegClass(DstReg)
                                         : TRI.getPhysRegClass(DstReg);

  return std::make_pair(SrcRC, DstRC);
}

static bool isVGPRToSGPRCopy(const TargetRegisterClass *SrcRC,
                             const TargetRegisterClass *DstRC,
                             const SIRegisterInfo &TRI) {
  return SrcRC != &AMDGPU::VReg_1RegClass && TRI.isSGPRClass(DstRC) &&
         TRI.hasVectorRegisters(SrcRC);
}

static bool isSGPRToVGPRCopy(const TargetRegisterClass *SrcRC,
                             const TargetRegisterClass *DstRC,
                             const SIRegisterInfo &TRI) {
  return DstRC != &AMDGPU::VReg_1RegClass && TRI.isSGPRClass(SrcRC) &&
         TRI.hasVectorRegisters(DstRC);
}

static bool tryChangeVGPRtoSGPRinCopy(MachineInstr &MI,
                                      const SIRegisterInfo *TRI,
                                      const SIInstrInfo *TII) {
  MachineRegisterInfo &MRI = MI.getParent()->getParent()->getRegInfo();
  auto &Src = MI.getOperand(1);
  Register DstReg = MI.getOperand(0).getReg();
  Register SrcReg = Src.getReg();
  if (!Register::isVirtualRegister(SrcReg) ||
      !Register::isVirtualRegister(DstReg))
    return false;

  for (const auto &MO : MRI.reg_nodbg_operands(DstReg)) {
    const auto *UseMI = MO.getParent();
    if (UseMI == &MI)
      continue;
    if (MO.isDef() || UseMI->getParent() != MI.getParent() ||
        UseMI->getOpcode() <= TargetOpcode::GENERIC_OP_END ||
        !TII->isOperandLegal(*UseMI, UseMI->getOperandNo(&MO), &Src))
      return false;
  }
  // Change VGPR to SGPR destination.
  MRI.setRegClass(DstReg, TRI->getEquivalentSGPRClass(MRI.getRegClass(DstReg)));
  return true;
}

// Distribute an SGPR->VGPR copy of a REG_SEQUENCE into a VGPR REG_SEQUENCE.
//
// SGPRx = ...
// SGPRy = REG_SEQUENCE SGPRx, sub0 ...
// VGPRz = COPY SGPRy
//
// ==>
//
// VGPRx = COPY SGPRx
// VGPRz = REG_SEQUENCE VGPRx, sub0
//
// This exposes immediate folding opportunities when materializing 64-bit
// immediates.
static bool foldVGPRCopyIntoRegSequence(MachineInstr &MI,
                                        const SIRegisterInfo *TRI,
                                        const SIInstrInfo *TII,
                                        MachineRegisterInfo &MRI) {
  assert(MI.isRegSequence());

  Register DstReg = MI.getOperand(0).getReg();
  if (!TRI->isSGPRClass(MRI.getRegClass(DstReg)))
    return false;

  if (!MRI.hasOneUse(DstReg))
    return false;

  MachineInstr &CopyUse = *MRI.use_instr_begin(DstReg);
  if (!CopyUse.isCopy())
    return false;

  // It is illegal to have vreg inputs to a physreg defining reg_sequence.
  if (Register::isPhysicalRegister(CopyUse.getOperand(0).getReg()))
    return false;

  const TargetRegisterClass *SrcRC, *DstRC;
  std::tie(SrcRC, DstRC) = getCopyRegClasses(CopyUse, *TRI, MRI);

  if (!isSGPRToVGPRCopy(SrcRC, DstRC, *TRI))
    return false;

  if (tryChangeVGPRtoSGPRinCopy(CopyUse, TRI, TII))
    return true;

  // TODO: Could have multiple extracts?
  unsigned SubReg = CopyUse.getOperand(1).getSubReg();
  if (SubReg != AMDGPU::NoSubRegister)
    return false;

  MRI.setRegClass(DstReg, DstRC);

  // SGPRx = ...
  // SGPRy = REG_SEQUENCE SGPRx, sub0 ...
  // VGPRz = COPY SGPRy

  // =>
  // VGPRx = COPY SGPRx
  // VGPRz = REG_SEQUENCE VGPRx, sub0

  MI.getOperand(0).setReg(CopyUse.getOperand(0).getReg());
  bool IsAGPR = TRI->hasAGPRs(DstRC);

  for (unsigned I = 1, N = MI.getNumOperands(); I != N; I += 2) {
    Register SrcReg = MI.getOperand(I).getReg();
    unsigned SrcSubReg = MI.getOperand(I).getSubReg();

    const TargetRegisterClass *SrcRC = MRI.getRegClass(SrcReg);
    assert(TRI->isSGPRClass(SrcRC) &&
           "Expected SGPR REG_SEQUENCE to only have SGPR inputs");

    SrcRC = TRI->getSubRegClass(SrcRC, SrcSubReg);
    const TargetRegisterClass *NewSrcRC = TRI->getEquivalentVGPRClass(SrcRC);

    Register TmpReg = MRI.createVirtualRegister(NewSrcRC);

    BuildMI(*MI.getParent(), &MI, MI.getDebugLoc(), TII->get(AMDGPU::COPY),
            TmpReg)
        .add(MI.getOperand(I));

    if (IsAGPR) {
      const TargetRegisterClass *NewSrcRC = TRI->getEquivalentAGPRClass(SrcRC);
      Register TmpAReg = MRI.createVirtualRegister(NewSrcRC);
      unsigned Opc = NewSrcRC == &AMDGPU::AGPR_32RegClass ?
        AMDGPU::V_ACCVGPR_WRITE_B32 : AMDGPU::COPY;
      BuildMI(*MI.getParent(), &MI, MI.getDebugLoc(), TII->get(Opc),
            TmpAReg)
        .addReg(TmpReg, RegState::Kill);
      TmpReg = TmpAReg;
    }

    MI.getOperand(I).setReg(TmpReg);
  }

  CopyUse.eraseFromParent();
  return true;
}

static bool isSafeToFoldImmIntoCopy(const MachineInstr *Copy,
                                    const MachineInstr *MoveImm,
                                    const SIInstrInfo *TII,
                                    unsigned &SMovOp,
                                    int64_t &Imm) {
  if (Copy->getOpcode() != AMDGPU::COPY)
    return false;

  if (!MoveImm->isMoveImmediate())
    return false;

  const MachineOperand *ImmOp =
      TII->getNamedOperand(*MoveImm, AMDGPU::OpName::src0);
  if (!ImmOp->isImm())
    return false;

  // FIXME: Handle copies with sub-regs.
  if (Copy->getOperand(0).getSubReg())
    return false;

  switch (MoveImm->getOpcode()) {
  default:
    return false;
  case AMDGPU::V_MOV_B32_e32:
    SMovOp = AMDGPU::S_MOV_B32;
    break;
  case AMDGPU::V_MOV_B64_PSEUDO:
    SMovOp = AMDGPU::S_MOV_B64;
    break;
  }
  Imm = ImmOp->getImm();
  return true;
}

template <class UnaryPredicate>
bool searchPredecessors(const MachineBasicBlock *MBB,
                        const MachineBasicBlock *CutOff,
                        UnaryPredicate Predicate) {
  if (MBB == CutOff)
    return false;

  DenseSet<const MachineBasicBlock *> Visited;
  SmallVector<MachineBasicBlock *, 4> Worklist(MBB->pred_begin(),
                                               MBB->pred_end());

  while (!Worklist.empty()) {
    MachineBasicBlock *MBB = Worklist.pop_back_val();

    if (!Visited.insert(MBB).second)
      continue;
    if (MBB == CutOff)
      continue;
    if (Predicate(MBB))
      return true;

    Worklist.append(MBB->pred_begin(), MBB->pred_end());
  }

  return false;
}

// Checks if there is potential path From instruction To instruction.
// If CutOff is specified and it sits in between of that path we ignore
// a higher portion of the path and report it is not reachable.
static bool isReachable(const MachineInstr *From,
                        const MachineInstr *To,
                        const MachineBasicBlock *CutOff,
                        MachineDominatorTree &MDT) {
  // If either From block dominates To block or instructions are in the same
  // block and From is higher.
  if (MDT.dominates(From, To))
    return true;

  const MachineBasicBlock *MBBFrom = From->getParent();
  const MachineBasicBlock *MBBTo = To->getParent();
  if (MBBFrom == MBBTo)
    return false;

  // Instructions are in different blocks, do predecessor search.
  // We should almost never get here since we do not usually produce M0 stores
  // other than -1.
  return searchPredecessors(MBBTo, CutOff, [MBBFrom]
           (const MachineBasicBlock *MBB) { return MBB == MBBFrom; });
}

// Return the first non-prologue instruction in the block.
static MachineBasicBlock::iterator
getFirstNonPrologue(MachineBasicBlock *MBB, const TargetInstrInfo *TII) {
  MachineBasicBlock::iterator I = MBB->getFirstNonPHI();
  while (I != MBB->end() && TII->isBasicBlockPrologue(*I))
    ++I;

  return I;
}

// Hoist and merge identical SGPR initializations into a common predecessor.
// This is intended to combine M0 initializations, but can work with any
// SGPR. A VGPR cannot be processed since we cannot guarantee vector
// executioon.
static bool hoistAndMergeSGPRInits(unsigned Reg,
                                   const MachineRegisterInfo &MRI,
                                   const TargetRegisterInfo *TRI,
                                   MachineDominatorTree &MDT,
                                   const TargetInstrInfo *TII) {
  // List of inits by immediate value.
  using InitListMap = std::map<unsigned, std::list<MachineInstr *>>;
  InitListMap Inits;
  // List of clobbering instructions.
  SmallVector<MachineInstr*, 8> Clobbers;
  // List of instructions marked for deletion.
  SmallSet<MachineInstr*, 8> MergedInstrs;

  bool Changed = false;

  for (auto &MI : MRI.def_instructions(Reg)) {
    MachineOperand *Imm = nullptr;
    for (auto &MO : MI.operands()) {
      if ((MO.isReg() && ((MO.isDef() && MO.getReg() != Reg) || !MO.isDef())) ||
          (!MO.isImm() && !MO.isReg()) || (MO.isImm() && Imm)) {
        Imm = nullptr;
        break;
      } else if (MO.isImm())
        Imm = &MO;
    }
    if (Imm)
      Inits[Imm->getImm()].push_front(&MI);
    else
      Clobbers.push_back(&MI);
  }

  for (auto &Init : Inits) {
    auto &Defs = Init.second;

    for (auto I1 = Defs.begin(), E = Defs.end(); I1 != E; ) {
      MachineInstr *MI1 = *I1;

      for (auto I2 = std::next(I1); I2 != E; ) {
        MachineInstr *MI2 = *I2;

        // Check any possible interference
        auto interferes = [&](MachineBasicBlock::iterator From,
                              MachineBasicBlock::iterator To) -> bool {

          assert(MDT.dominates(&*To, &*From));

          auto interferes = [&MDT, From, To](MachineInstr* &Clobber) -> bool {
            const MachineBasicBlock *MBBFrom = From->getParent();
            const MachineBasicBlock *MBBTo = To->getParent();
            bool MayClobberFrom = isReachable(Clobber, &*From, MBBTo, MDT);
            bool MayClobberTo = isReachable(Clobber, &*To, MBBTo, MDT);
            if (!MayClobberFrom && !MayClobberTo)
              return false;
            if ((MayClobberFrom && !MayClobberTo) ||
                (!MayClobberFrom && MayClobberTo))
              return true;
            // Both can clobber, this is not an interference only if both are
            // dominated by Clobber and belong to the same block or if Clobber
            // properly dominates To, given that To >> From, so it dominates
            // both and located in a common dominator.
            return !((MBBFrom == MBBTo &&
                      MDT.dominates(Clobber, &*From) &&
                      MDT.dominates(Clobber, &*To)) ||
                     MDT.properlyDominates(Clobber->getParent(), MBBTo));
          };

          return (llvm::any_of(Clobbers, interferes)) ||
                 (llvm::any_of(Inits, [&](InitListMap::value_type &C) {
                    return C.first != Init.first &&
                           llvm::any_of(C.second, interferes);
                  }));
        };

        if (MDT.dominates(MI1, MI2)) {
          if (!interferes(MI2, MI1)) {
            LLVM_DEBUG(dbgs()
                       << "Erasing from "
                       << printMBBReference(*MI2->getParent()) << " " << *MI2);
            MergedInstrs.insert(MI2);
            Changed = true;
            ++I2;
            continue;
          }
        } else if (MDT.dominates(MI2, MI1)) {
          if (!interferes(MI1, MI2)) {
            LLVM_DEBUG(dbgs()
                       << "Erasing from "
                       << printMBBReference(*MI1->getParent()) << " " << *MI1);
            MergedInstrs.insert(MI1);
            Changed = true;
            ++I1;
            break;
          }
        } else {
          auto *MBB = MDT.findNearestCommonDominator(MI1->getParent(),
                                                     MI2->getParent());
          if (!MBB) {
            ++I2;
            continue;
          }

          MachineBasicBlock::iterator I = getFirstNonPrologue(MBB, TII);
          if (!interferes(MI1, I) && !interferes(MI2, I)) {
            LLVM_DEBUG(dbgs()
                       << "Erasing from "
                       << printMBBReference(*MI1->getParent()) << " " << *MI1
                       << "and moving from "
                       << printMBBReference(*MI2->getParent()) << " to "
                       << printMBBReference(*I->getParent()) << " " << *MI2);
            I->getParent()->splice(I, MI2->getParent(), MI2);
            MergedInstrs.insert(MI1);
            Changed = true;
            ++I1;
            break;
          }
        }
        ++I2;
      }
      ++I1;
    }
  }

  // Remove initializations that were merged into another.
  for (auto &Init : Inits) {
    auto &Defs = Init.second;
    auto I = Defs.begin();
    while (I != Defs.end()) {
      if (MergedInstrs.count(*I)) {
        (*I)->eraseFromParent();
        I = Defs.erase(I);
      } else
        ++I;
    }
  }

  // Try to schedule SGPR initializations as early as possible in the MBB.
  for (auto &Init : Inits) {
    auto &Defs = Init.second;
    for (auto MI : Defs) {
      auto MBB = MI->getParent();
      MachineInstr &BoundaryMI = *getFirstNonPrologue(MBB, TII);
      MachineBasicBlock::reverse_iterator B(BoundaryMI);
      // Check if B should actually be a boundary. If not set the previous
      // instruction as the boundary instead.
      if (!TII->isBasicBlockPrologue(*B))
        B++;

      auto R = std::next(MI->getReverseIterator());
      const unsigned Threshold = 50;
      // Search until B or Threshold for a place to insert the initialization.
      for (unsigned I = 0; R != B && I < Threshold; ++R, ++I)
        if (R->readsRegister(Reg, TRI) || R->definesRegister(Reg, TRI) ||
            TII->isSchedulingBoundary(*R, MBB, *MBB->getParent()))
          break;

      // Move to directly after R.
      if (&*--R != MI)
        MBB->splice(*R, MBB, MI);
    }
  }

  if (Changed)
    MRI.clearKillFlags(Reg);

  return Changed;
}

bool SIFixSGPRCopies::runOnMachineFunction(MachineFunction &MF) {
  const GCNSubtarget &ST = MF.getSubtarget<GCNSubtarget>();
  MRI = &MF.getRegInfo();
  TRI = ST.getRegisterInfo();
  TII = ST.getInstrInfo();
  MDT = &getAnalysis<MachineDominatorTree>();

  SmallVector<MachineInstr *, 16> Worklist;

  for (MachineFunction::iterator BI = MF.begin(), BE = MF.end();
                                                  BI != BE; ++BI) {
    MachineBasicBlock &MBB = *BI;
    for (MachineBasicBlock::iterator I = MBB.begin(), E = MBB.end();
         I != E; ++I) {
      MachineInstr &MI = *I;

      switch (MI.getOpcode()) {
      default:
        continue;
      case AMDGPU::COPY:
      case AMDGPU::WQM:
      case AMDGPU::SOFT_WQM:
      case AMDGPU::WWM: {
        Register DstReg = MI.getOperand(0).getReg();

        const TargetRegisterClass *SrcRC, *DstRC;
        std::tie(SrcRC, DstRC) = getCopyRegClasses(MI, *TRI, *MRI);

        if (!Register::isVirtualRegister(DstReg)) {
          // If the destination register is a physical register there isn't
          // really much we can do to fix this.
          // Some special instructions use M0 as an input. Some even only use
          // the first lane. Insert a readfirstlane and hope for the best.
          if (DstReg == AMDGPU::M0 && TRI->hasVectorRegisters(SrcRC)) {
            Register TmpReg
              = MRI->createVirtualRegister(&AMDGPU::SReg_32_XM0RegClass);

            BuildMI(MBB, MI, MI.getDebugLoc(),
                    TII->get(AMDGPU::V_READFIRSTLANE_B32), TmpReg)
              .add(MI.getOperand(1));
            MI.getOperand(1).setReg(TmpReg);
          }

          continue;
        }

        if (isVGPRToSGPRCopy(SrcRC, DstRC, *TRI)) {
          Register SrcReg = MI.getOperand(1).getReg();
          if (!Register::isVirtualRegister(SrcReg)) {
            TII->moveToVALU(MI, MDT);
            break;
          }

          MachineInstr *DefMI = MRI->getVRegDef(SrcReg);
          unsigned SMovOp;
          int64_t Imm;
          // If we are just copying an immediate, we can replace the copy with
          // s_mov_b32.
          if (isSafeToFoldImmIntoCopy(&MI, DefMI, TII, SMovOp, Imm)) {
            MI.getOperand(1).ChangeToImmediate(Imm);
            MI.addImplicitDefUseOperands(MF);
            MI.setDesc(TII->get(SMovOp));
            break;
          }
          TII->moveToVALU(MI, MDT);
        } else if (isSGPRToVGPRCopy(SrcRC, DstRC, *TRI)) {
          tryChangeVGPRtoSGPRinCopy(MI, TRI, TII);
        }

        break;
      }
      case AMDGPU::PHI: {
        processPHINode(MI);
        break;
      }
      case AMDGPU::REG_SEQUENCE:
        if (TRI->hasVectorRegisters(TII->getOpRegClass(MI, 0)) ||
            !hasVectorOperands(MI, TRI)) {
          foldVGPRCopyIntoRegSequence(MI, TRI, TII, *MRI);
          continue;
        }

        LLVM_DEBUG(dbgs() << "Fixing REG_SEQUENCE: " << MI);

        TII->moveToVALU(MI, MDT);
        break;
      case AMDGPU::INSERT_SUBREG: {
        const TargetRegisterClass *DstRC, *Src0RC, *Src1RC;
        DstRC = MRI->getRegClass(MI.getOperand(0).getReg());
        Src0RC = MRI->getRegClass(MI.getOperand(1).getReg());
        Src1RC = MRI->getRegClass(MI.getOperand(2).getReg());
        if (TRI->isSGPRClass(DstRC) &&
            (TRI->hasVectorRegisters(Src0RC) ||
             TRI->hasVectorRegisters(Src1RC))) {
          LLVM_DEBUG(dbgs() << " Fixing INSERT_SUBREG: " << MI);
          TII->moveToVALU(MI, MDT);
        }
        break;
      }
      }
    }
  }

  if (MF.getTarget().getOptLevel() > CodeGenOpt::None && EnableM0Merge)
    hoistAndMergeSGPRInits(AMDGPU::M0, *MRI, TRI, *MDT, TII);

  return true;
}

void SIFixSGPRCopies::processPHINode(MachineInstr &MI) {
  unsigned numVGPRUses = 0;
  SetVector<const MachineInstr *> worklist;
  worklist.insert(&MI);
  while (!worklist.empty()) {
    const MachineInstr *Instr = worklist.pop_back_val();
    unsigned Reg = Instr->getOperand(0).getReg();
    for (const auto &Use : MRI->use_operands(Reg)) {
      const MachineInstr *UseMI = Use.getParent();
      if (UseMI->isCopy() || UseMI->isRegSequence()) {
        if (UseMI->isCopy() &&
          UseMI->getOperand(0).getReg().isPhysical() &&
          !TRI->isSGPRReg(*MRI, UseMI->getOperand(0).getReg())) {
          numVGPRUses++;
        }
        worklist.insert(UseMI);
        continue;
      }

      if (UseMI->isPHI()) {
        const TargetRegisterClass *UseRC = MRI->getRegClass(Use.getReg());
        if (!TRI->isSGPRReg(*MRI, Use.getReg()) &&
          UseRC != &AMDGPU::VReg_1RegClass)
          numVGPRUses++;
        continue;
      }

      const TargetRegisterClass *OpRC =
        TII->getOpRegClass(*UseMI, UseMI->getOperandNo(&Use));
      if (!TRI->isSGPRClass(OpRC) && OpRC != &AMDGPU::VS_32RegClass &&
        OpRC != &AMDGPU::VS_64RegClass) {
        numVGPRUses++;
      }
    }
  }
  bool hasVGPRInput = false;
  for (unsigned i = 1; i < MI.getNumOperands(); i += 2) {
    unsigned InputReg = MI.getOperand(i).getReg();
    MachineInstr *Def = MRI->getVRegDef(InputReg);
    if (TRI->isVGPR(*MRI, InputReg)) {
      if (Def->isCopy()) {
        unsigned SrcReg = Def->getOperand(1).getReg();
        const TargetRegisterClass *RC =
          TRI->getRegClassForReg(*MRI, SrcReg);
        if (TRI->isSGPRClass(RC))
          continue;
      }
      hasVGPRInput = true;
      break;
    }
    else if (Def->isCopy() &&
      TRI->isVGPR(*MRI, Def->getOperand(1).getReg())) {
      hasVGPRInput = true;
      break;
    }
  }
  unsigned PHIRes = MI.getOperand(0).getReg();
  const TargetRegisterClass *RC0 = MRI->getRegClass(PHIRes);

  if ((!TRI->isVGPR(*MRI, PHIRes) && RC0 != &AMDGPU::VReg_1RegClass) &&
    (hasVGPRInput || numVGPRUses > 1)) {
    LLVM_DEBUG(dbgs() << "Fixing PHI: " << MI);
    TII->moveToVALU(MI);
  }
  else {
    LLVM_DEBUG(dbgs() << "Legalizing PHI: " << MI);
    TII->legalizeOperands(MI, MDT);
  }

}
