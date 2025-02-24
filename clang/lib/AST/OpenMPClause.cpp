//===- OpenMPClause.cpp - Classes for OpenMP clauses ----------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file implements the subclesses of Stmt class declared in OpenMPClause.h
//
//===----------------------------------------------------------------------===//

#include "clang/AST/OpenMPClause.h"
#include "clang/AST/ASTContext.h"
#include "clang/AST/Decl.h"
#include "clang/AST/DeclOpenMP.h"
#include "clang/Basic/LLVM.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/Support/Casting.h"
#include "llvm/Support/ErrorHandling.h"
#include <algorithm>
#include <cassert>

using namespace clang;

OMPClause::child_range OMPClause::children() {
  switch (getClauseKind()) {
  default:
    break;
#define OPENMP_CLAUSE(Name, Class)                                             \
  case OMPC_##Name:                                                            \
    return static_cast<Class *>(this)->children();
#include "clang/Basic/OpenMPKinds.def"
  }
  llvm_unreachable("unknown OMPClause");
}

OMPClause::child_range OMPClause::used_children() {
  switch (getClauseKind()) {
#define OPENMP_CLAUSE(Name, Class)                                             \
  case OMPC_##Name:                                                            \
    return static_cast<Class *>(this)->used_children();
#include "clang/Basic/OpenMPKinds.def"
  case OMPC_threadprivate:
  case OMPC_uniform:
  case OMPC_device_type:
  case OMPC_match:
  case OMPC_unknown:
    break;
  }
  llvm_unreachable("unknown OMPClause");
}

OMPClauseWithPreInit *OMPClauseWithPreInit::get(OMPClause *C) {
  auto *Res = OMPClauseWithPreInit::get(const_cast<const OMPClause *>(C));
  return Res ? const_cast<OMPClauseWithPreInit *>(Res) : nullptr;
}

const OMPClauseWithPreInit *OMPClauseWithPreInit::get(const OMPClause *C) {
  switch (C->getClauseKind()) {
  case OMPC_schedule:
    return static_cast<const OMPScheduleClause *>(C);
  case OMPC_dist_schedule:
    return static_cast<const OMPDistScheduleClause *>(C);
  case OMPC_firstprivate:
    return static_cast<const OMPFirstprivateClause *>(C);
  case OMPC_lastprivate:
    return static_cast<const OMPLastprivateClause *>(C);
  case OMPC_reduction:
    return static_cast<const OMPReductionClause *>(C);
  case OMPC_task_reduction:
    return static_cast<const OMPTaskReductionClause *>(C);
  case OMPC_in_reduction:
    return static_cast<const OMPInReductionClause *>(C);
  case OMPC_linear:
    return static_cast<const OMPLinearClause *>(C);
  case OMPC_if:
    return static_cast<const OMPIfClause *>(C);
  case OMPC_num_threads:
    return static_cast<const OMPNumThreadsClause *>(C);
  case OMPC_num_teams:
    return static_cast<const OMPNumTeamsClause *>(C);
  case OMPC_thread_limit:
    return static_cast<const OMPThreadLimitClause *>(C);
  case OMPC_device:
    return static_cast<const OMPDeviceClause *>(C);
  case OMPC_grainsize:
    return static_cast<const OMPGrainsizeClause *>(C);
  case OMPC_default:
  case OMPC_proc_bind:
  case OMPC_final:
  case OMPC_safelen:
  case OMPC_simdlen:
  case OMPC_allocator:
  case OMPC_allocate:
  case OMPC_collapse:
  case OMPC_private:
  case OMPC_shared:
  case OMPC_aligned:
  case OMPC_copyin:
  case OMPC_copyprivate:
  case OMPC_ordered:
  case OMPC_nowait:
  case OMPC_untied:
  case OMPC_mergeable:
  case OMPC_threadprivate:
  case OMPC_flush:
  case OMPC_read:
  case OMPC_write:
  case OMPC_update:
  case OMPC_capture:
  case OMPC_seq_cst:
  case OMPC_depend:
  case OMPC_threads:
  case OMPC_simd:
  case OMPC_map:
  case OMPC_priority:
  case OMPC_nogroup:
  case OMPC_num_tasks:
  case OMPC_hint:
  case OMPC_defaultmap:
  case OMPC_unknown:
  case OMPC_uniform:
  case OMPC_to:
  case OMPC_from:
  case OMPC_use_device_ptr:
  case OMPC_is_device_ptr:
  case OMPC_unified_address:
  case OMPC_unified_shared_memory:
  case OMPC_reverse_offload:
  case OMPC_dynamic_allocators:
  case OMPC_atomic_default_mem_order:
  case OMPC_device_type:
  case OMPC_match:
    break;
  }

  return nullptr;
}

OMPClauseWithPostUpdate *OMPClauseWithPostUpdate::get(OMPClause *C) {
  auto *Res = OMPClauseWithPostUpdate::get(const_cast<const OMPClause *>(C));
  return Res ? const_cast<OMPClauseWithPostUpdate *>(Res) : nullptr;
}

const OMPClauseWithPostUpdate *OMPClauseWithPostUpdate::get(const OMPClause *C) {
  switch (C->getClauseKind()) {
  case OMPC_lastprivate:
    return static_cast<const OMPLastprivateClause *>(C);
  case OMPC_reduction:
    return static_cast<const OMPReductionClause *>(C);
  case OMPC_task_reduction:
    return static_cast<const OMPTaskReductionClause *>(C);
  case OMPC_in_reduction:
    return static_cast<const OMPInReductionClause *>(C);
  case OMPC_linear:
    return static_cast<const OMPLinearClause *>(C);
  case OMPC_schedule:
  case OMPC_dist_schedule:
  case OMPC_firstprivate:
  case OMPC_default:
  case OMPC_proc_bind:
  case OMPC_if:
  case OMPC_final:
  case OMPC_num_threads:
  case OMPC_safelen:
  case OMPC_simdlen:
  case OMPC_allocator:
  case OMPC_allocate:
  case OMPC_collapse:
  case OMPC_private:
  case OMPC_shared:
  case OMPC_aligned:
  case OMPC_copyin:
  case OMPC_copyprivate:
  case OMPC_ordered:
  case OMPC_nowait:
  case OMPC_untied:
  case OMPC_mergeable:
  case OMPC_threadprivate:
  case OMPC_flush:
  case OMPC_read:
  case OMPC_write:
  case OMPC_update:
  case OMPC_capture:
  case OMPC_seq_cst:
  case OMPC_depend:
  case OMPC_device:
  case OMPC_threads:
  case OMPC_simd:
  case OMPC_map:
  case OMPC_num_teams:
  case OMPC_thread_limit:
  case OMPC_priority:
  case OMPC_grainsize:
  case OMPC_nogroup:
  case OMPC_num_tasks:
  case OMPC_hint:
  case OMPC_defaultmap:
  case OMPC_unknown:
  case OMPC_uniform:
  case OMPC_to:
  case OMPC_from:
  case OMPC_use_device_ptr:
  case OMPC_is_device_ptr:
  case OMPC_unified_address:
  case OMPC_unified_shared_memory:
  case OMPC_reverse_offload:
  case OMPC_dynamic_allocators:
  case OMPC_atomic_default_mem_order:
  case OMPC_device_type:
  case OMPC_match:
    break;
  }

  return nullptr;
}

/// Gets the address of the original, non-captured, expression used in the
/// clause as the preinitializer.
static Stmt **getAddrOfExprAsWritten(Stmt *S) {
  if (!S)
    return nullptr;
  if (auto *DS = dyn_cast<DeclStmt>(S)) {
    assert(DS->isSingleDecl() && "Only single expression must be captured.");
    if (auto *OED = dyn_cast<OMPCapturedExprDecl>(DS->getSingleDecl()))
      return OED->getInitAddress();
  }
  return nullptr;
}

OMPClause::child_range OMPIfClause::used_children() {
  if (Stmt **C = getAddrOfExprAsWritten(getPreInitStmt()))
    return child_range(C, C + 1);
  return child_range(&Condition, &Condition + 1);
}

OMPClause::child_range OMPGrainsizeClause::used_children() {
  if (Stmt **C = getAddrOfExprAsWritten(getPreInitStmt()))
    return child_range(C, C + 1);
  return child_range(&Grainsize, &Grainsize + 1);
}

OMPOrderedClause *OMPOrderedClause::Create(const ASTContext &C, Expr *Num,
                                           unsigned NumLoops,
                                           SourceLocation StartLoc,
                                           SourceLocation LParenLoc,
                                           SourceLocation EndLoc) {
  void *Mem = C.Allocate(totalSizeToAlloc<Expr *>(2 * NumLoops));
  auto *Clause =
      new (Mem) OMPOrderedClause(Num, NumLoops, StartLoc, LParenLoc, EndLoc);
  for (unsigned I = 0; I < NumLoops; ++I) {
    Clause->setLoopNumIterations(I, nullptr);
    Clause->setLoopCounter(I, nullptr);
  }
  return Clause;
}

OMPOrderedClause *OMPOrderedClause::CreateEmpty(const ASTContext &C,
                                                unsigned NumLoops) {
  void *Mem = C.Allocate(totalSizeToAlloc<Expr *>(2 * NumLoops));
  auto *Clause = new (Mem) OMPOrderedClause(NumLoops);
  for (unsigned I = 0; I < NumLoops; ++I) {
    Clause->setLoopNumIterations(I, nullptr);
    Clause->setLoopCounter(I, nullptr);
  }
  return Clause;
}

void OMPOrderedClause::setLoopNumIterations(unsigned NumLoop,
                                            Expr *NumIterations) {
  assert(NumLoop < NumberOfLoops && "out of loops number.");
  getTrailingObjects<Expr *>()[NumLoop] = NumIterations;
}

ArrayRef<Expr *> OMPOrderedClause::getLoopNumIterations() const {
  return llvm::makeArrayRef(getTrailingObjects<Expr *>(), NumberOfLoops);
}

void OMPOrderedClause::setLoopCounter(unsigned NumLoop, Expr *Counter) {
  assert(NumLoop < NumberOfLoops && "out of loops number.");
  getTrailingObjects<Expr *>()[NumberOfLoops + NumLoop] = Counter;
}

Expr *OMPOrderedClause::getLoopCounter(unsigned NumLoop) {
  assert(NumLoop < NumberOfLoops && "out of loops number.");
  return getTrailingObjects<Expr *>()[NumberOfLoops + NumLoop];
}

const Expr *OMPOrderedClause::getLoopCounter(unsigned NumLoop) const {
  assert(NumLoop < NumberOfLoops && "out of loops number.");
  return getTrailingObjects<Expr *>()[NumberOfLoops + NumLoop];
}

void OMPPrivateClause::setPrivateCopies(ArrayRef<Expr *> VL) {
  assert(VL.size() == varlist_size() &&
         "Number of private copies is not the same as the preallocated buffer");
  std::copy(VL.begin(), VL.end(), varlist_end());
}

OMPPrivateClause *
OMPPrivateClause::Create(const ASTContext &C, SourceLocation StartLoc,
                         SourceLocation LParenLoc, SourceLocation EndLoc,
                         ArrayRef<Expr *> VL, ArrayRef<Expr *> PrivateVL) {
  // Allocate space for private variables and initializer expressions.
  void *Mem = C.Allocate(totalSizeToAlloc<Expr *>(2 * VL.size()));
  OMPPrivateClause *Clause =
      new (Mem) OMPPrivateClause(StartLoc, LParenLoc, EndLoc, VL.size());
  Clause->setVarRefs(VL);
  Clause->setPrivateCopies(PrivateVL);
  return Clause;
}

OMPPrivateClause *OMPPrivateClause::CreateEmpty(const ASTContext &C,
                                                unsigned N) {
  void *Mem = C.Allocate(totalSizeToAlloc<Expr *>(2 * N));
  return new (Mem) OMPPrivateClause(N);
}

void OMPFirstprivateClause::setPrivateCopies(ArrayRef<Expr *> VL) {
  assert(VL.size() == varlist_size() &&
         "Number of private copies is not the same as the preallocated buffer");
  std::copy(VL.begin(), VL.end(), varlist_end());
}

void OMPFirstprivateClause::setInits(ArrayRef<Expr *> VL) {
  assert(VL.size() == varlist_size() &&
         "Number of inits is not the same as the preallocated buffer");
  std::copy(VL.begin(), VL.end(), getPrivateCopies().end());
}

OMPFirstprivateClause *
OMPFirstprivateClause::Create(const ASTContext &C, SourceLocation StartLoc,
                              SourceLocation LParenLoc, SourceLocation EndLoc,
                              ArrayRef<Expr *> VL, ArrayRef<Expr *> PrivateVL,
                              ArrayRef<Expr *> InitVL, Stmt *PreInit) {
  void *Mem = C.Allocate(totalSizeToAlloc<Expr *>(3 * VL.size()));
  OMPFirstprivateClause *Clause =
      new (Mem) OMPFirstprivateClause(StartLoc, LParenLoc, EndLoc, VL.size());
  Clause->setVarRefs(VL);
  Clause->setPrivateCopies(PrivateVL);
  Clause->setInits(InitVL);
  Clause->setPreInitStmt(PreInit);
  return Clause;
}

OMPFirstprivateClause *OMPFirstprivateClause::CreateEmpty(const ASTContext &C,
                                                          unsigned N) {
  void *Mem = C.Allocate(totalSizeToAlloc<Expr *>(3 * N));
  return new (Mem) OMPFirstprivateClause(N);
}

void OMPLastprivateClause::setPrivateCopies(ArrayRef<Expr *> PrivateCopies) {
  assert(PrivateCopies.size() == varlist_size() &&
         "Number of private copies is not the same as the preallocated buffer");
  std::copy(PrivateCopies.begin(), PrivateCopies.end(), varlist_end());
}

void OMPLastprivateClause::setSourceExprs(ArrayRef<Expr *> SrcExprs) {
  assert(SrcExprs.size() == varlist_size() && "Number of source expressions is "
                                              "not the same as the "
                                              "preallocated buffer");
  std::copy(SrcExprs.begin(), SrcExprs.end(), getPrivateCopies().end());
}

void OMPLastprivateClause::setDestinationExprs(ArrayRef<Expr *> DstExprs) {
  assert(DstExprs.size() == varlist_size() && "Number of destination "
                                              "expressions is not the same as "
                                              "the preallocated buffer");
  std::copy(DstExprs.begin(), DstExprs.end(), getSourceExprs().end());
}

void OMPLastprivateClause::setAssignmentOps(ArrayRef<Expr *> AssignmentOps) {
  assert(AssignmentOps.size() == varlist_size() &&
         "Number of assignment expressions is not the same as the preallocated "
         "buffer");
  std::copy(AssignmentOps.begin(), AssignmentOps.end(),
            getDestinationExprs().end());
}

OMPLastprivateClause *OMPLastprivateClause::Create(
    const ASTContext &C, SourceLocation StartLoc, SourceLocation LParenLoc,
    SourceLocation EndLoc, ArrayRef<Expr *> VL, ArrayRef<Expr *> SrcExprs,
    ArrayRef<Expr *> DstExprs, ArrayRef<Expr *> AssignmentOps, Stmt *PreInit,
    Expr *PostUpdate) {
  void *Mem = C.Allocate(totalSizeToAlloc<Expr *>(5 * VL.size()));
  OMPLastprivateClause *Clause =
      new (Mem) OMPLastprivateClause(StartLoc, LParenLoc, EndLoc, VL.size());
  Clause->setVarRefs(VL);
  Clause->setSourceExprs(SrcExprs);
  Clause->setDestinationExprs(DstExprs);
  Clause->setAssignmentOps(AssignmentOps);
  Clause->setPreInitStmt(PreInit);
  Clause->setPostUpdateExpr(PostUpdate);
  return Clause;
}

OMPLastprivateClause *OMPLastprivateClause::CreateEmpty(const ASTContext &C,
                                                        unsigned N) {
  void *Mem = C.Allocate(totalSizeToAlloc<Expr *>(5 * N));
  return new (Mem) OMPLastprivateClause(N);
}

OMPSharedClause *OMPSharedClause::Create(const ASTContext &C,
                                         SourceLocation StartLoc,
                                         SourceLocation LParenLoc,
                                         SourceLocation EndLoc,
                                         ArrayRef<Expr *> VL) {
  void *Mem = C.Allocate(totalSizeToAlloc<Expr *>(VL.size()));
  OMPSharedClause *Clause =
      new (Mem) OMPSharedClause(StartLoc, LParenLoc, EndLoc, VL.size());
  Clause->setVarRefs(VL);
  return Clause;
}

OMPSharedClause *OMPSharedClause::CreateEmpty(const ASTContext &C, unsigned N) {
  void *Mem = C.Allocate(totalSizeToAlloc<Expr *>(N));
  return new (Mem) OMPSharedClause(N);
}

void OMPLinearClause::setPrivates(ArrayRef<Expr *> PL) {
  assert(PL.size() == varlist_size() &&
         "Number of privates is not the same as the preallocated buffer");
  std::copy(PL.begin(), PL.end(), varlist_end());
}

void OMPLinearClause::setInits(ArrayRef<Expr *> IL) {
  assert(IL.size() == varlist_size() &&
         "Number of inits is not the same as the preallocated buffer");
  std::copy(IL.begin(), IL.end(), getPrivates().end());
}

void OMPLinearClause::setUpdates(ArrayRef<Expr *> UL) {
  assert(UL.size() == varlist_size() &&
         "Number of updates is not the same as the preallocated buffer");
  std::copy(UL.begin(), UL.end(), getInits().end());
}

void OMPLinearClause::setFinals(ArrayRef<Expr *> FL) {
  assert(FL.size() == varlist_size() &&
         "Number of final updates is not the same as the preallocated buffer");
  std::copy(FL.begin(), FL.end(), getUpdates().end());
}

void OMPLinearClause::setUsedExprs(ArrayRef<Expr *> UE) {
  assert(
      UE.size() == varlist_size() + 1 &&
      "Number of used expressions is not the same as the preallocated buffer");
  std::copy(UE.begin(), UE.end(), getFinals().end() + 2);
}

OMPLinearClause *OMPLinearClause::Create(
    const ASTContext &C, SourceLocation StartLoc, SourceLocation LParenLoc,
    OpenMPLinearClauseKind Modifier, SourceLocation ModifierLoc,
    SourceLocation ColonLoc, SourceLocation EndLoc, ArrayRef<Expr *> VL,
    ArrayRef<Expr *> PL, ArrayRef<Expr *> IL, Expr *Step, Expr *CalcStep,
    Stmt *PreInit, Expr *PostUpdate) {
  // Allocate space for 5 lists (Vars, Inits, Updates, Finals), 2 expressions
  // (Step and CalcStep), list of used expression + step.
  void *Mem =
      C.Allocate(totalSizeToAlloc<Expr *>(5 * VL.size() + 2 + VL.size() + 1));
  OMPLinearClause *Clause = new (Mem) OMPLinearClause(
      StartLoc, LParenLoc, Modifier, ModifierLoc, ColonLoc, EndLoc, VL.size());
  Clause->setVarRefs(VL);
  Clause->setPrivates(PL);
  Clause->setInits(IL);
  // Fill update and final expressions with zeroes, they are provided later,
  // after the directive construction.
  std::fill(Clause->getInits().end(), Clause->getInits().end() + VL.size(),
            nullptr);
  std::fill(Clause->getUpdates().end(), Clause->getUpdates().end() + VL.size(),
            nullptr);
  std::fill(Clause->getUsedExprs().begin(), Clause->getUsedExprs().end(),
            nullptr);
  Clause->setStep(Step);
  Clause->setCalcStep(CalcStep);
  Clause->setPreInitStmt(PreInit);
  Clause->setPostUpdateExpr(PostUpdate);
  return Clause;
}

OMPLinearClause *OMPLinearClause::CreateEmpty(const ASTContext &C,
                                              unsigned NumVars) {
  // Allocate space for 5 lists (Vars, Inits, Updates, Finals), 2 expressions
  // (Step and CalcStep), list of used expression + step.
  void *Mem = C.Allocate(totalSizeToAlloc<Expr *>(5 * NumVars + 2 + NumVars  +1));
  return new (Mem) OMPLinearClause(NumVars);
}

OMPClause::child_range OMPLinearClause::used_children() {
  // Range includes only non-nullptr elements.
  return child_range(
      reinterpret_cast<Stmt **>(getUsedExprs().begin()),
      reinterpret_cast<Stmt **>(llvm::find(getUsedExprs(), nullptr)));
}

OMPAlignedClause *
OMPAlignedClause::Create(const ASTContext &C, SourceLocation StartLoc,
                         SourceLocation LParenLoc, SourceLocation ColonLoc,
                         SourceLocation EndLoc, ArrayRef<Expr *> VL, Expr *A) {
  void *Mem = C.Allocate(totalSizeToAlloc<Expr *>(VL.size() + 1));
  OMPAlignedClause *Clause = new (Mem)
      OMPAlignedClause(StartLoc, LParenLoc, ColonLoc, EndLoc, VL.size());
  Clause->setVarRefs(VL);
  Clause->setAlignment(A);
  return Clause;
}

OMPAlignedClause *OMPAlignedClause::CreateEmpty(const ASTContext &C,
                                                unsigned NumVars) {
  void *Mem = C.Allocate(totalSizeToAlloc<Expr *>(NumVars + 1));
  return new (Mem) OMPAlignedClause(NumVars);
}

void OMPCopyinClause::setSourceExprs(ArrayRef<Expr *> SrcExprs) {
  assert(SrcExprs.size() == varlist_size() && "Number of source expressions is "
                                              "not the same as the "
                                              "preallocated buffer");
  std::copy(SrcExprs.begin(), SrcExprs.end(), varlist_end());
}

void OMPCopyinClause::setDestinationExprs(ArrayRef<Expr *> DstExprs) {
  assert(DstExprs.size() == varlist_size() && "Number of destination "
                                              "expressions is not the same as "
                                              "the preallocated buffer");
  std::copy(DstExprs.begin(), DstExprs.end(), getSourceExprs().end());
}

void OMPCopyinClause::setAssignmentOps(ArrayRef<Expr *> AssignmentOps) {
  assert(AssignmentOps.size() == varlist_size() &&
         "Number of assignment expressions is not the same as the preallocated "
         "buffer");
  std::copy(AssignmentOps.begin(), AssignmentOps.end(),
            getDestinationExprs().end());
}

OMPCopyinClause *OMPCopyinClause::Create(
    const ASTContext &C, SourceLocation StartLoc, SourceLocation LParenLoc,
    SourceLocation EndLoc, ArrayRef<Expr *> VL, ArrayRef<Expr *> SrcExprs,
    ArrayRef<Expr *> DstExprs, ArrayRef<Expr *> AssignmentOps) {
  void *Mem = C.Allocate(totalSizeToAlloc<Expr *>(4 * VL.size()));
  OMPCopyinClause *Clause =
      new (Mem) OMPCopyinClause(StartLoc, LParenLoc, EndLoc, VL.size());
  Clause->setVarRefs(VL);
  Clause->setSourceExprs(SrcExprs);
  Clause->setDestinationExprs(DstExprs);
  Clause->setAssignmentOps(AssignmentOps);
  return Clause;
}

OMPCopyinClause *OMPCopyinClause::CreateEmpty(const ASTContext &C, unsigned N) {
  void *Mem = C.Allocate(totalSizeToAlloc<Expr *>(4 * N));
  return new (Mem) OMPCopyinClause(N);
}

void OMPCopyprivateClause::setSourceExprs(ArrayRef<Expr *> SrcExprs) {
  assert(SrcExprs.size() == varlist_size() && "Number of source expressions is "
                                              "not the same as the "
                                              "preallocated buffer");
  std::copy(SrcExprs.begin(), SrcExprs.end(), varlist_end());
}

void OMPCopyprivateClause::setDestinationExprs(ArrayRef<Expr *> DstExprs) {
  assert(DstExprs.size() == varlist_size() && "Number of destination "
                                              "expressions is not the same as "
                                              "the preallocated buffer");
  std::copy(DstExprs.begin(), DstExprs.end(), getSourceExprs().end());
}

void OMPCopyprivateClause::setAssignmentOps(ArrayRef<Expr *> AssignmentOps) {
  assert(AssignmentOps.size() == varlist_size() &&
         "Number of assignment expressions is not the same as the preallocated "
         "buffer");
  std::copy(AssignmentOps.begin(), AssignmentOps.end(),
            getDestinationExprs().end());
}

OMPCopyprivateClause *OMPCopyprivateClause::Create(
    const ASTContext &C, SourceLocation StartLoc, SourceLocation LParenLoc,
    SourceLocation EndLoc, ArrayRef<Expr *> VL, ArrayRef<Expr *> SrcExprs,
    ArrayRef<Expr *> DstExprs, ArrayRef<Expr *> AssignmentOps) {
  void *Mem = C.Allocate(totalSizeToAlloc<Expr *>(4 * VL.size()));
  OMPCopyprivateClause *Clause =
      new (Mem) OMPCopyprivateClause(StartLoc, LParenLoc, EndLoc, VL.size());
  Clause->setVarRefs(VL);
  Clause->setSourceExprs(SrcExprs);
  Clause->setDestinationExprs(DstExprs);
  Clause->setAssignmentOps(AssignmentOps);
  return Clause;
}

OMPCopyprivateClause *OMPCopyprivateClause::CreateEmpty(const ASTContext &C,
                                                        unsigned N) {
  void *Mem = C.Allocate(totalSizeToAlloc<Expr *>(4 * N));
  return new (Mem) OMPCopyprivateClause(N);
}

void OMPReductionClause::setPrivates(ArrayRef<Expr *> Privates) {
  assert(Privates.size() == varlist_size() &&
         "Number of private copies is not the same as the preallocated buffer");
  std::copy(Privates.begin(), Privates.end(), varlist_end());
}

void OMPReductionClause::setLHSExprs(ArrayRef<Expr *> LHSExprs) {
  assert(
      LHSExprs.size() == varlist_size() &&
      "Number of LHS expressions is not the same as the preallocated buffer");
  std::copy(LHSExprs.begin(), LHSExprs.end(), getPrivates().end());
}

void OMPReductionClause::setRHSExprs(ArrayRef<Expr *> RHSExprs) {
  assert(
      RHSExprs.size() == varlist_size() &&
      "Number of RHS expressions is not the same as the preallocated buffer");
  std::copy(RHSExprs.begin(), RHSExprs.end(), getLHSExprs().end());
}

void OMPReductionClause::setReductionOps(ArrayRef<Expr *> ReductionOps) {
  assert(ReductionOps.size() == varlist_size() && "Number of reduction "
                                                  "expressions is not the same "
                                                  "as the preallocated buffer");
  std::copy(ReductionOps.begin(), ReductionOps.end(), getRHSExprs().end());
}

OMPReductionClause *OMPReductionClause::Create(
    const ASTContext &C, SourceLocation StartLoc, SourceLocation LParenLoc,
    SourceLocation EndLoc, SourceLocation ColonLoc, ArrayRef<Expr *> VL,
    NestedNameSpecifierLoc QualifierLoc, const DeclarationNameInfo &NameInfo,
    ArrayRef<Expr *> Privates, ArrayRef<Expr *> LHSExprs,
    ArrayRef<Expr *> RHSExprs, ArrayRef<Expr *> ReductionOps, Stmt *PreInit,
    Expr *PostUpdate) {
  void *Mem = C.Allocate(totalSizeToAlloc<Expr *>(5 * VL.size()));
  OMPReductionClause *Clause = new (Mem) OMPReductionClause(
      StartLoc, LParenLoc, EndLoc, ColonLoc, VL.size(), QualifierLoc, NameInfo);
  Clause->setVarRefs(VL);
  Clause->setPrivates(Privates);
  Clause->setLHSExprs(LHSExprs);
  Clause->setRHSExprs(RHSExprs);
  Clause->setReductionOps(ReductionOps);
  Clause->setPreInitStmt(PreInit);
  Clause->setPostUpdateExpr(PostUpdate);
  return Clause;
}

OMPReductionClause *OMPReductionClause::CreateEmpty(const ASTContext &C,
                                                    unsigned N) {
  void *Mem = C.Allocate(totalSizeToAlloc<Expr *>(5 * N));
  return new (Mem) OMPReductionClause(N);
}

void OMPTaskReductionClause::setPrivates(ArrayRef<Expr *> Privates) {
  assert(Privates.size() == varlist_size() &&
         "Number of private copies is not the same as the preallocated buffer");
  std::copy(Privates.begin(), Privates.end(), varlist_end());
}

void OMPTaskReductionClause::setLHSExprs(ArrayRef<Expr *> LHSExprs) {
  assert(
      LHSExprs.size() == varlist_size() &&
      "Number of LHS expressions is not the same as the preallocated buffer");
  std::copy(LHSExprs.begin(), LHSExprs.end(), getPrivates().end());
}

void OMPTaskReductionClause::setRHSExprs(ArrayRef<Expr *> RHSExprs) {
  assert(
      RHSExprs.size() == varlist_size() &&
      "Number of RHS expressions is not the same as the preallocated buffer");
  std::copy(RHSExprs.begin(), RHSExprs.end(), getLHSExprs().end());
}

void OMPTaskReductionClause::setReductionOps(ArrayRef<Expr *> ReductionOps) {
  assert(ReductionOps.size() == varlist_size() && "Number of task reduction "
                                                  "expressions is not the same "
                                                  "as the preallocated buffer");
  std::copy(ReductionOps.begin(), ReductionOps.end(), getRHSExprs().end());
}

OMPTaskReductionClause *OMPTaskReductionClause::Create(
    const ASTContext &C, SourceLocation StartLoc, SourceLocation LParenLoc,
    SourceLocation EndLoc, SourceLocation ColonLoc, ArrayRef<Expr *> VL,
    NestedNameSpecifierLoc QualifierLoc, const DeclarationNameInfo &NameInfo,
    ArrayRef<Expr *> Privates, ArrayRef<Expr *> LHSExprs,
    ArrayRef<Expr *> RHSExprs, ArrayRef<Expr *> ReductionOps, Stmt *PreInit,
    Expr *PostUpdate) {
  void *Mem = C.Allocate(totalSizeToAlloc<Expr *>(5 * VL.size()));
  OMPTaskReductionClause *Clause = new (Mem) OMPTaskReductionClause(
      StartLoc, LParenLoc, EndLoc, ColonLoc, VL.size(), QualifierLoc, NameInfo);
  Clause->setVarRefs(VL);
  Clause->setPrivates(Privates);
  Clause->setLHSExprs(LHSExprs);
  Clause->setRHSExprs(RHSExprs);
  Clause->setReductionOps(ReductionOps);
  Clause->setPreInitStmt(PreInit);
  Clause->setPostUpdateExpr(PostUpdate);
  return Clause;
}

OMPTaskReductionClause *OMPTaskReductionClause::CreateEmpty(const ASTContext &C,
                                                            unsigned N) {
  void *Mem = C.Allocate(totalSizeToAlloc<Expr *>(5 * N));
  return new (Mem) OMPTaskReductionClause(N);
}

void OMPInReductionClause::setPrivates(ArrayRef<Expr *> Privates) {
  assert(Privates.size() == varlist_size() &&
         "Number of private copies is not the same as the preallocated buffer");
  std::copy(Privates.begin(), Privates.end(), varlist_end());
}

void OMPInReductionClause::setLHSExprs(ArrayRef<Expr *> LHSExprs) {
  assert(
      LHSExprs.size() == varlist_size() &&
      "Number of LHS expressions is not the same as the preallocated buffer");
  std::copy(LHSExprs.begin(), LHSExprs.end(), getPrivates().end());
}

void OMPInReductionClause::setRHSExprs(ArrayRef<Expr *> RHSExprs) {
  assert(
      RHSExprs.size() == varlist_size() &&
      "Number of RHS expressions is not the same as the preallocated buffer");
  std::copy(RHSExprs.begin(), RHSExprs.end(), getLHSExprs().end());
}

void OMPInReductionClause::setReductionOps(ArrayRef<Expr *> ReductionOps) {
  assert(ReductionOps.size() == varlist_size() && "Number of in reduction "
                                                  "expressions is not the same "
                                                  "as the preallocated buffer");
  std::copy(ReductionOps.begin(), ReductionOps.end(), getRHSExprs().end());
}

void OMPInReductionClause::setTaskgroupDescriptors(
    ArrayRef<Expr *> TaskgroupDescriptors) {
  assert(TaskgroupDescriptors.size() == varlist_size() &&
         "Number of in reduction descriptors is not the same as the "
         "preallocated buffer");
  std::copy(TaskgroupDescriptors.begin(), TaskgroupDescriptors.end(),
            getReductionOps().end());
}

OMPInReductionClause *OMPInReductionClause::Create(
    const ASTContext &C, SourceLocation StartLoc, SourceLocation LParenLoc,
    SourceLocation EndLoc, SourceLocation ColonLoc, ArrayRef<Expr *> VL,
    NestedNameSpecifierLoc QualifierLoc, const DeclarationNameInfo &NameInfo,
    ArrayRef<Expr *> Privates, ArrayRef<Expr *> LHSExprs,
    ArrayRef<Expr *> RHSExprs, ArrayRef<Expr *> ReductionOps,
    ArrayRef<Expr *> TaskgroupDescriptors, Stmt *PreInit, Expr *PostUpdate) {
  void *Mem = C.Allocate(totalSizeToAlloc<Expr *>(6 * VL.size()));
  OMPInReductionClause *Clause = new (Mem) OMPInReductionClause(
      StartLoc, LParenLoc, EndLoc, ColonLoc, VL.size(), QualifierLoc, NameInfo);
  Clause->setVarRefs(VL);
  Clause->setPrivates(Privates);
  Clause->setLHSExprs(LHSExprs);
  Clause->setRHSExprs(RHSExprs);
  Clause->setReductionOps(ReductionOps);
  Clause->setTaskgroupDescriptors(TaskgroupDescriptors);
  Clause->setPreInitStmt(PreInit);
  Clause->setPostUpdateExpr(PostUpdate);
  return Clause;
}

OMPInReductionClause *OMPInReductionClause::CreateEmpty(const ASTContext &C,
                                                        unsigned N) {
  void *Mem = C.Allocate(totalSizeToAlloc<Expr *>(6 * N));
  return new (Mem) OMPInReductionClause(N);
}

OMPAllocateClause *
OMPAllocateClause::Create(const ASTContext &C, SourceLocation StartLoc,
                          SourceLocation LParenLoc, Expr *Allocator,
                          SourceLocation ColonLoc, SourceLocation EndLoc,
                          ArrayRef<Expr *> VL) {
  // Allocate space for private variables and initializer expressions.
  void *Mem = C.Allocate(totalSizeToAlloc<Expr *>(VL.size()));
  auto *Clause = new (Mem) OMPAllocateClause(StartLoc, LParenLoc, Allocator,
                                             ColonLoc, EndLoc, VL.size());
  Clause->setVarRefs(VL);
  return Clause;
}

OMPAllocateClause *OMPAllocateClause::CreateEmpty(const ASTContext &C,
                                                  unsigned N) {
  void *Mem = C.Allocate(totalSizeToAlloc<Expr *>(N));
  return new (Mem) OMPAllocateClause(N);
}

OMPFlushClause *OMPFlushClause::Create(const ASTContext &C,
                                       SourceLocation StartLoc,
                                       SourceLocation LParenLoc,
                                       SourceLocation EndLoc,
                                       ArrayRef<Expr *> VL) {
  void *Mem = C.Allocate(totalSizeToAlloc<Expr *>(VL.size() + 1));
  OMPFlushClause *Clause =
      new (Mem) OMPFlushClause(StartLoc, LParenLoc, EndLoc, VL.size());
  Clause->setVarRefs(VL);
  return Clause;
}

OMPFlushClause *OMPFlushClause::CreateEmpty(const ASTContext &C, unsigned N) {
  void *Mem = C.Allocate(totalSizeToAlloc<Expr *>(N));
  return new (Mem) OMPFlushClause(N);
}

OMPDependClause *
OMPDependClause::Create(const ASTContext &C, SourceLocation StartLoc,
                        SourceLocation LParenLoc, SourceLocation EndLoc,
                        OpenMPDependClauseKind DepKind, SourceLocation DepLoc,
                        SourceLocation ColonLoc, ArrayRef<Expr *> VL,
                        unsigned NumLoops) {
  void *Mem = C.Allocate(totalSizeToAlloc<Expr *>(VL.size() + NumLoops));
  OMPDependClause *Clause = new (Mem)
      OMPDependClause(StartLoc, LParenLoc, EndLoc, VL.size(), NumLoops);
  Clause->setVarRefs(VL);
  Clause->setDependencyKind(DepKind);
  Clause->setDependencyLoc(DepLoc);
  Clause->setColonLoc(ColonLoc);
  for (unsigned I = 0 ; I < NumLoops; ++I)
    Clause->setLoopData(I, nullptr);
  return Clause;
}

OMPDependClause *OMPDependClause::CreateEmpty(const ASTContext &C, unsigned N,
                                              unsigned NumLoops) {
  void *Mem = C.Allocate(totalSizeToAlloc<Expr *>(N + NumLoops));
  return new (Mem) OMPDependClause(N, NumLoops);
}

void OMPDependClause::setLoopData(unsigned NumLoop, Expr *Cnt) {
  assert((getDependencyKind() == OMPC_DEPEND_sink ||
          getDependencyKind() == OMPC_DEPEND_source) &&
         NumLoop < NumLoops &&
         "Expected sink or source depend + loop index must be less number of "
         "loops.");
  auto It = std::next(getVarRefs().end(), NumLoop);
  *It = Cnt;
}

Expr *OMPDependClause::getLoopData(unsigned NumLoop) {
  assert((getDependencyKind() == OMPC_DEPEND_sink ||
          getDependencyKind() == OMPC_DEPEND_source) &&
         NumLoop < NumLoops &&
         "Expected sink or source depend + loop index must be less number of "
         "loops.");
  auto It = std::next(getVarRefs().end(), NumLoop);
  return *It;
}

const Expr *OMPDependClause::getLoopData(unsigned NumLoop) const {
  assert((getDependencyKind() == OMPC_DEPEND_sink ||
          getDependencyKind() == OMPC_DEPEND_source) &&
         NumLoop < NumLoops &&
         "Expected sink or source depend + loop index must be less number of "
         "loops.");
  auto It = std::next(getVarRefs().end(), NumLoop);
  return *It;
}

unsigned OMPClauseMappableExprCommon::getComponentsTotalNumber(
    MappableExprComponentListsRef ComponentLists) {
  unsigned TotalNum = 0u;
  for (auto &C : ComponentLists)
    TotalNum += C.size();
  return TotalNum;
}

unsigned OMPClauseMappableExprCommon::getUniqueDeclarationsTotalNumber(
    ArrayRef<const ValueDecl *> Declarations) {
  unsigned TotalNum = 0u;
  llvm::SmallPtrSet<const ValueDecl *, 8> Cache;
  for (const ValueDecl *D : Declarations) {
    const ValueDecl *VD = D ? cast<ValueDecl>(D->getCanonicalDecl()) : nullptr;
    if (Cache.count(VD))
      continue;
    ++TotalNum;
    Cache.insert(VD);
  }
  return TotalNum;
}

OMPMapClause *OMPMapClause::Create(
    const ASTContext &C, const OMPVarListLocTy &Locs, ArrayRef<Expr *> Vars,
    ArrayRef<ValueDecl *> Declarations,
    MappableExprComponentListsRef ComponentLists, ArrayRef<Expr *> UDMapperRefs,
    ArrayRef<OpenMPMapModifierKind> MapModifiers,
    ArrayRef<SourceLocation> MapModifiersLoc,
    NestedNameSpecifierLoc UDMQualifierLoc, DeclarationNameInfo MapperId,
    OpenMPMapClauseKind Type, bool TypeIsImplicit, SourceLocation TypeLoc) {
  OMPMappableExprListSizeTy Sizes;
  Sizes.NumVars = Vars.size();
  Sizes.NumUniqueDeclarations = getUniqueDeclarationsTotalNumber(Declarations);
  Sizes.NumComponentLists = ComponentLists.size();
  Sizes.NumComponents = getComponentsTotalNumber(ComponentLists);

  // We need to allocate:
  // 2 x NumVars x Expr* - we have an original list expression and an associated
  // user-defined mapper for each clause list entry.
  // NumUniqueDeclarations x ValueDecl* - unique base declarations associated
  // with each component list.
  // (NumUniqueDeclarations + NumComponentLists) x unsigned - we specify the
  // number of lists for each unique declaration and the size of each component
  // list.
  // NumComponents x MappableComponent - the total of all the components in all
  // the lists.
  void *Mem = C.Allocate(
      totalSizeToAlloc<Expr *, ValueDecl *, unsigned,
                       OMPClauseMappableExprCommon::MappableComponent>(
          2 * Sizes.NumVars, Sizes.NumUniqueDeclarations,
          Sizes.NumUniqueDeclarations + Sizes.NumComponentLists,
          Sizes.NumComponents));
  OMPMapClause *Clause = new (Mem)
      OMPMapClause(MapModifiers, MapModifiersLoc, UDMQualifierLoc, MapperId,
                   Type, TypeIsImplicit, TypeLoc, Locs, Sizes);

  Clause->setVarRefs(Vars);
  Clause->setUDMapperRefs(UDMapperRefs);
  Clause->setClauseInfo(Declarations, ComponentLists);
  Clause->setMapType(Type);
  Clause->setMapLoc(TypeLoc);
  return Clause;
}

OMPMapClause *
OMPMapClause::CreateEmpty(const ASTContext &C,
                          const OMPMappableExprListSizeTy &Sizes) {
  void *Mem = C.Allocate(
      totalSizeToAlloc<Expr *, ValueDecl *, unsigned,
                       OMPClauseMappableExprCommon::MappableComponent>(
          2 * Sizes.NumVars, Sizes.NumUniqueDeclarations,
          Sizes.NumUniqueDeclarations + Sizes.NumComponentLists,
          Sizes.NumComponents));
  return new (Mem) OMPMapClause(Sizes);
}

OMPToClause *OMPToClause::Create(
    const ASTContext &C, const OMPVarListLocTy &Locs, ArrayRef<Expr *> Vars,
    ArrayRef<ValueDecl *> Declarations,
    MappableExprComponentListsRef ComponentLists, ArrayRef<Expr *> UDMapperRefs,
    NestedNameSpecifierLoc UDMQualifierLoc, DeclarationNameInfo MapperId) {
  OMPMappableExprListSizeTy Sizes;
  Sizes.NumVars = Vars.size();
  Sizes.NumUniqueDeclarations = getUniqueDeclarationsTotalNumber(Declarations);
  Sizes.NumComponentLists = ComponentLists.size();
  Sizes.NumComponents = getComponentsTotalNumber(ComponentLists);

  // We need to allocate:
  // 2 x NumVars x Expr* - we have an original list expression and an associated
  // user-defined mapper for each clause list entry.
  // NumUniqueDeclarations x ValueDecl* - unique base declarations associated
  // with each component list.
  // (NumUniqueDeclarations + NumComponentLists) x unsigned - we specify the
  // number of lists for each unique declaration and the size of each component
  // list.
  // NumComponents x MappableComponent - the total of all the components in all
  // the lists.
  void *Mem = C.Allocate(
      totalSizeToAlloc<Expr *, ValueDecl *, unsigned,
                       OMPClauseMappableExprCommon::MappableComponent>(
          2 * Sizes.NumVars, Sizes.NumUniqueDeclarations,
          Sizes.NumUniqueDeclarations + Sizes.NumComponentLists,
          Sizes.NumComponents));

  auto *Clause = new (Mem) OMPToClause(UDMQualifierLoc, MapperId, Locs, Sizes);

  Clause->setVarRefs(Vars);
  Clause->setUDMapperRefs(UDMapperRefs);
  Clause->setClauseInfo(Declarations, ComponentLists);
  return Clause;
}

OMPToClause *OMPToClause::CreateEmpty(const ASTContext &C,
                                      const OMPMappableExprListSizeTy &Sizes) {
  void *Mem = C.Allocate(
      totalSizeToAlloc<Expr *, ValueDecl *, unsigned,
                       OMPClauseMappableExprCommon::MappableComponent>(
          2 * Sizes.NumVars, Sizes.NumUniqueDeclarations,
          Sizes.NumUniqueDeclarations + Sizes.NumComponentLists,
          Sizes.NumComponents));
  return new (Mem) OMPToClause(Sizes);
}

OMPFromClause *OMPFromClause::Create(
    const ASTContext &C, const OMPVarListLocTy &Locs, ArrayRef<Expr *> Vars,
    ArrayRef<ValueDecl *> Declarations,
    MappableExprComponentListsRef ComponentLists, ArrayRef<Expr *> UDMapperRefs,
    NestedNameSpecifierLoc UDMQualifierLoc, DeclarationNameInfo MapperId) {
  OMPMappableExprListSizeTy Sizes;
  Sizes.NumVars = Vars.size();
  Sizes.NumUniqueDeclarations = getUniqueDeclarationsTotalNumber(Declarations);
  Sizes.NumComponentLists = ComponentLists.size();
  Sizes.NumComponents = getComponentsTotalNumber(ComponentLists);

  // We need to allocate:
  // 2 x NumVars x Expr* - we have an original list expression and an associated
  // user-defined mapper for each clause list entry.
  // NumUniqueDeclarations x ValueDecl* - unique base declarations associated
  // with each component list.
  // (NumUniqueDeclarations + NumComponentLists) x unsigned - we specify the
  // number of lists for each unique declaration and the size of each component
  // list.
  // NumComponents x MappableComponent - the total of all the components in all
  // the lists.
  void *Mem = C.Allocate(
      totalSizeToAlloc<Expr *, ValueDecl *, unsigned,
                       OMPClauseMappableExprCommon::MappableComponent>(
          2 * Sizes.NumVars, Sizes.NumUniqueDeclarations,
          Sizes.NumUniqueDeclarations + Sizes.NumComponentLists,
          Sizes.NumComponents));

  auto *Clause =
      new (Mem) OMPFromClause(UDMQualifierLoc, MapperId, Locs, Sizes);

  Clause->setVarRefs(Vars);
  Clause->setUDMapperRefs(UDMapperRefs);
  Clause->setClauseInfo(Declarations, ComponentLists);
  return Clause;
}

OMPFromClause *
OMPFromClause::CreateEmpty(const ASTContext &C,
                           const OMPMappableExprListSizeTy &Sizes) {
  void *Mem = C.Allocate(
      totalSizeToAlloc<Expr *, ValueDecl *, unsigned,
                       OMPClauseMappableExprCommon::MappableComponent>(
          2 * Sizes.NumVars, Sizes.NumUniqueDeclarations,
          Sizes.NumUniqueDeclarations + Sizes.NumComponentLists,
          Sizes.NumComponents));
  return new (Mem) OMPFromClause(Sizes);
}

void OMPUseDevicePtrClause::setPrivateCopies(ArrayRef<Expr *> VL) {
  assert(VL.size() == varlist_size() &&
         "Number of private copies is not the same as the preallocated buffer");
  std::copy(VL.begin(), VL.end(), varlist_end());
}

void OMPUseDevicePtrClause::setInits(ArrayRef<Expr *> VL) {
  assert(VL.size() == varlist_size() &&
         "Number of inits is not the same as the preallocated buffer");
  std::copy(VL.begin(), VL.end(), getPrivateCopies().end());
}

OMPUseDevicePtrClause *OMPUseDevicePtrClause::Create(
    const ASTContext &C, const OMPVarListLocTy &Locs, ArrayRef<Expr *> Vars,
    ArrayRef<Expr *> PrivateVars, ArrayRef<Expr *> Inits,
    ArrayRef<ValueDecl *> Declarations,
    MappableExprComponentListsRef ComponentLists) {
  OMPMappableExprListSizeTy Sizes;
  Sizes.NumVars = Vars.size();
  Sizes.NumUniqueDeclarations = getUniqueDeclarationsTotalNumber(Declarations);
  Sizes.NumComponentLists = ComponentLists.size();
  Sizes.NumComponents = getComponentsTotalNumber(ComponentLists);

  // We need to allocate:
  // 3 x NumVars x Expr* - we have an original list expression for each clause
  // list entry and an equal number of private copies and inits.
  // NumUniqueDeclarations x ValueDecl* - unique base declarations associated
  // with each component list.
  // (NumUniqueDeclarations + NumComponentLists) x unsigned - we specify the
  // number of lists for each unique declaration and the size of each component
  // list.
  // NumComponents x MappableComponent - the total of all the components in all
  // the lists.
  void *Mem = C.Allocate(
      totalSizeToAlloc<Expr *, ValueDecl *, unsigned,
                       OMPClauseMappableExprCommon::MappableComponent>(
          3 * Sizes.NumVars, Sizes.NumUniqueDeclarations,
          Sizes.NumUniqueDeclarations + Sizes.NumComponentLists,
          Sizes.NumComponents));

  OMPUseDevicePtrClause *Clause = new (Mem) OMPUseDevicePtrClause(Locs, Sizes);

  Clause->setVarRefs(Vars);
  Clause->setPrivateCopies(PrivateVars);
  Clause->setInits(Inits);
  Clause->setClauseInfo(Declarations, ComponentLists);
  return Clause;
}

OMPUseDevicePtrClause *
OMPUseDevicePtrClause::CreateEmpty(const ASTContext &C,
                                   const OMPMappableExprListSizeTy &Sizes) {
  void *Mem = C.Allocate(
      totalSizeToAlloc<Expr *, ValueDecl *, unsigned,
                       OMPClauseMappableExprCommon::MappableComponent>(
          3 * Sizes.NumVars, Sizes.NumUniqueDeclarations,
          Sizes.NumUniqueDeclarations + Sizes.NumComponentLists,
          Sizes.NumComponents));
  return new (Mem) OMPUseDevicePtrClause(Sizes);
}

OMPIsDevicePtrClause *
OMPIsDevicePtrClause::Create(const ASTContext &C, const OMPVarListLocTy &Locs,
                             ArrayRef<Expr *> Vars,
                             ArrayRef<ValueDecl *> Declarations,
                             MappableExprComponentListsRef ComponentLists) {
  OMPMappableExprListSizeTy Sizes;
  Sizes.NumVars = Vars.size();
  Sizes.NumUniqueDeclarations = getUniqueDeclarationsTotalNumber(Declarations);
  Sizes.NumComponentLists = ComponentLists.size();
  Sizes.NumComponents = getComponentsTotalNumber(ComponentLists);

  // We need to allocate:
  // NumVars x Expr* - we have an original list expression for each clause list
  // entry.
  // NumUniqueDeclarations x ValueDecl* - unique base declarations associated
  // with each component list.
  // (NumUniqueDeclarations + NumComponentLists) x unsigned - we specify the
  // number of lists for each unique declaration and the size of each component
  // list.
  // NumComponents x MappableComponent - the total of all the components in all
  // the lists.
  void *Mem = C.Allocate(
      totalSizeToAlloc<Expr *, ValueDecl *, unsigned,
                       OMPClauseMappableExprCommon::MappableComponent>(
          Sizes.NumVars, Sizes.NumUniqueDeclarations,
          Sizes.NumUniqueDeclarations + Sizes.NumComponentLists,
          Sizes.NumComponents));

  OMPIsDevicePtrClause *Clause = new (Mem) OMPIsDevicePtrClause(Locs, Sizes);

  Clause->setVarRefs(Vars);
  Clause->setClauseInfo(Declarations, ComponentLists);
  return Clause;
}

OMPIsDevicePtrClause *
OMPIsDevicePtrClause::CreateEmpty(const ASTContext &C,
                                  const OMPMappableExprListSizeTy &Sizes) {
  void *Mem = C.Allocate(
      totalSizeToAlloc<Expr *, ValueDecl *, unsigned,
                       OMPClauseMappableExprCommon::MappableComponent>(
          Sizes.NumVars, Sizes.NumUniqueDeclarations,
          Sizes.NumUniqueDeclarations + Sizes.NumComponentLists,
          Sizes.NumComponents));
  return new (Mem) OMPIsDevicePtrClause(Sizes);
}

//===----------------------------------------------------------------------===//
//  OpenMP clauses printing methods
//===----------------------------------------------------------------------===//

void OMPClausePrinter::VisitOMPIfClause(OMPIfClause *Node) {
  OS << "if(";
  if (Node->getNameModifier() != OMPD_unknown)
    OS << getOpenMPDirectiveName(Node->getNameModifier()) << ": ";
  Node->getCondition()->printPretty(OS, nullptr, Policy, 0);
  OS << ")";
}

void OMPClausePrinter::VisitOMPFinalClause(OMPFinalClause *Node) {
  OS << "final(";
  Node->getCondition()->printPretty(OS, nullptr, Policy, 0);
  OS << ")";
}

void OMPClausePrinter::VisitOMPNumThreadsClause(OMPNumThreadsClause *Node) {
  OS << "num_threads(";
  Node->getNumThreads()->printPretty(OS, nullptr, Policy, 0);
  OS << ")";
}

void OMPClausePrinter::VisitOMPSafelenClause(OMPSafelenClause *Node) {
  OS << "safelen(";
  Node->getSafelen()->printPretty(OS, nullptr, Policy, 0);
  OS << ")";
}

void OMPClausePrinter::VisitOMPSimdlenClause(OMPSimdlenClause *Node) {
  OS << "simdlen(";
  Node->getSimdlen()->printPretty(OS, nullptr, Policy, 0);
  OS << ")";
}

void OMPClausePrinter::VisitOMPAllocatorClause(OMPAllocatorClause *Node) {
  OS << "allocator(";
  Node->getAllocator()->printPretty(OS, nullptr, Policy, 0);
  OS << ")";
}

void OMPClausePrinter::VisitOMPCollapseClause(OMPCollapseClause *Node) {
  OS << "collapse(";
  Node->getNumForLoops()->printPretty(OS, nullptr, Policy, 0);
  OS << ")";
}

void OMPClausePrinter::VisitOMPDefaultClause(OMPDefaultClause *Node) {
  OS << "default("
     << getOpenMPSimpleClauseTypeName(OMPC_default, Node->getDefaultKind())
     << ")";
}

void OMPClausePrinter::VisitOMPProcBindClause(OMPProcBindClause *Node) {
  OS << "proc_bind("
     << getOpenMPSimpleClauseTypeName(OMPC_proc_bind, Node->getProcBindKind())
     << ")";
}

void OMPClausePrinter::VisitOMPUnifiedAddressClause(OMPUnifiedAddressClause *) {
  OS << "unified_address";
}

void OMPClausePrinter::VisitOMPUnifiedSharedMemoryClause(
    OMPUnifiedSharedMemoryClause *) {
  OS << "unified_shared_memory";
}

void OMPClausePrinter::VisitOMPReverseOffloadClause(OMPReverseOffloadClause *) {
  OS << "reverse_offload";
}

void OMPClausePrinter::VisitOMPDynamicAllocatorsClause(
    OMPDynamicAllocatorsClause *) {
  OS << "dynamic_allocators";
}

void OMPClausePrinter::VisitOMPAtomicDefaultMemOrderClause(
    OMPAtomicDefaultMemOrderClause *Node) {
  OS << "atomic_default_mem_order("
     << getOpenMPSimpleClauseTypeName(OMPC_atomic_default_mem_order,
                                      Node->getAtomicDefaultMemOrderKind())
     << ")";
}

void OMPClausePrinter::VisitOMPScheduleClause(OMPScheduleClause *Node) {
  OS << "schedule(";
  if (Node->getFirstScheduleModifier() != OMPC_SCHEDULE_MODIFIER_unknown) {
    OS << getOpenMPSimpleClauseTypeName(OMPC_schedule,
                                        Node->getFirstScheduleModifier());
    if (Node->getSecondScheduleModifier() != OMPC_SCHEDULE_MODIFIER_unknown) {
      OS << ", ";
      OS << getOpenMPSimpleClauseTypeName(OMPC_schedule,
                                          Node->getSecondScheduleModifier());
    }
    OS << ": ";
  }
  OS << getOpenMPSimpleClauseTypeName(OMPC_schedule, Node->getScheduleKind());
  if (auto *E = Node->getChunkSize()) {
    OS << ", ";
    E->printPretty(OS, nullptr, Policy);
  }
  OS << ")";
}

void OMPClausePrinter::VisitOMPOrderedClause(OMPOrderedClause *Node) {
  OS << "ordered";
  if (auto *Num = Node->getNumForLoops()) {
    OS << "(";
    Num->printPretty(OS, nullptr, Policy, 0);
    OS << ")";
  }
}

void OMPClausePrinter::VisitOMPNowaitClause(OMPNowaitClause *) {
  OS << "nowait";
}

void OMPClausePrinter::VisitOMPUntiedClause(OMPUntiedClause *) {
  OS << "untied";
}

void OMPClausePrinter::VisitOMPNogroupClause(OMPNogroupClause *) {
  OS << "nogroup";
}

void OMPClausePrinter::VisitOMPMergeableClause(OMPMergeableClause *) {
  OS << "mergeable";
}

void OMPClausePrinter::VisitOMPReadClause(OMPReadClause *) { OS << "read"; }

void OMPClausePrinter::VisitOMPWriteClause(OMPWriteClause *) { OS << "write"; }

void OMPClausePrinter::VisitOMPUpdateClause(OMPUpdateClause *) {
  OS << "update";
}

void OMPClausePrinter::VisitOMPCaptureClause(OMPCaptureClause *) {
  OS << "capture";
}

void OMPClausePrinter::VisitOMPSeqCstClause(OMPSeqCstClause *) {
  OS << "seq_cst";
}

void OMPClausePrinter::VisitOMPThreadsClause(OMPThreadsClause *) {
  OS << "threads";
}

void OMPClausePrinter::VisitOMPSIMDClause(OMPSIMDClause *) { OS << "simd"; }

void OMPClausePrinter::VisitOMPDeviceClause(OMPDeviceClause *Node) {
  OS << "device(";
  Node->getDevice()->printPretty(OS, nullptr, Policy, 0);
  OS << ")";
}

void OMPClausePrinter::VisitOMPNumTeamsClause(OMPNumTeamsClause *Node) {
  OS << "num_teams(";
  Node->getNumTeams()->printPretty(OS, nullptr, Policy, 0);
  OS << ")";
}

void OMPClausePrinter::VisitOMPThreadLimitClause(OMPThreadLimitClause *Node) {
  OS << "thread_limit(";
  Node->getThreadLimit()->printPretty(OS, nullptr, Policy, 0);
  OS << ")";
}

void OMPClausePrinter::VisitOMPPriorityClause(OMPPriorityClause *Node) {
  OS << "priority(";
  Node->getPriority()->printPretty(OS, nullptr, Policy, 0);
  OS << ")";
}

void OMPClausePrinter::VisitOMPGrainsizeClause(OMPGrainsizeClause *Node) {
  OS << "grainsize(";
  Node->getGrainsize()->printPretty(OS, nullptr, Policy, 0);
  OS << ")";
}

void OMPClausePrinter::VisitOMPNumTasksClause(OMPNumTasksClause *Node) {
  OS << "num_tasks(";
  Node->getNumTasks()->printPretty(OS, nullptr, Policy, 0);
  OS << ")";
}

void OMPClausePrinter::VisitOMPHintClause(OMPHintClause *Node) {
  OS << "hint(";
  Node->getHint()->printPretty(OS, nullptr, Policy, 0);
  OS << ")";
}

template<typename T>
void OMPClausePrinter::VisitOMPClauseList(T *Node, char StartSym) {
  for (typename T::varlist_iterator I = Node->varlist_begin(),
                                    E = Node->varlist_end();
       I != E; ++I) {
    assert(*I && "Expected non-null Stmt");
    OS << (I == Node->varlist_begin() ? StartSym : ',');
    if (auto *DRE = dyn_cast<DeclRefExpr>(*I)) {
      if (isa<OMPCapturedExprDecl>(DRE->getDecl()))
        DRE->printPretty(OS, nullptr, Policy, 0);
      else
        DRE->getDecl()->printQualifiedName(OS);
    } else
      (*I)->printPretty(OS, nullptr, Policy, 0);
  }
}

void OMPClausePrinter::VisitOMPAllocateClause(OMPAllocateClause *Node) {
  if (Node->varlist_empty())
    return;
  OS << "allocate";
  if (Expr *Allocator = Node->getAllocator()) {
    OS << "(";
    Allocator->printPretty(OS, nullptr, Policy, 0);
    OS << ":";
    VisitOMPClauseList(Node, ' ');
  } else {
    VisitOMPClauseList(Node, '(');
  }
  OS << ")";
}

void OMPClausePrinter::VisitOMPPrivateClause(OMPPrivateClause *Node) {
  if (!Node->varlist_empty()) {
    OS << "private";
    VisitOMPClauseList(Node, '(');
    OS << ")";
  }
}

void OMPClausePrinter::VisitOMPFirstprivateClause(OMPFirstprivateClause *Node) {
  if (!Node->varlist_empty()) {
    OS << "firstprivate";
    VisitOMPClauseList(Node, '(');
    OS << ")";
  }
}

void OMPClausePrinter::VisitOMPLastprivateClause(OMPLastprivateClause *Node) {
  if (!Node->varlist_empty()) {
    OS << "lastprivate";
    VisitOMPClauseList(Node, '(');
    OS << ")";
  }
}

void OMPClausePrinter::VisitOMPSharedClause(OMPSharedClause *Node) {
  if (!Node->varlist_empty()) {
    OS << "shared";
    VisitOMPClauseList(Node, '(');
    OS << ")";
  }
}

void OMPClausePrinter::VisitOMPReductionClause(OMPReductionClause *Node) {
  if (!Node->varlist_empty()) {
    OS << "reduction(";
    NestedNameSpecifier *QualifierLoc =
        Node->getQualifierLoc().getNestedNameSpecifier();
    OverloadedOperatorKind OOK =
        Node->getNameInfo().getName().getCXXOverloadedOperator();
    if (QualifierLoc == nullptr && OOK != OO_None) {
      // Print reduction identifier in C format
      OS << getOperatorSpelling(OOK);
    } else {
      // Use C++ format
      if (QualifierLoc != nullptr)
        QualifierLoc->print(OS, Policy);
      OS << Node->getNameInfo();
    }
    OS << ":";
    VisitOMPClauseList(Node, ' ');
    OS << ")";
  }
}

void OMPClausePrinter::VisitOMPTaskReductionClause(
    OMPTaskReductionClause *Node) {
  if (!Node->varlist_empty()) {
    OS << "task_reduction(";
    NestedNameSpecifier *QualifierLoc =
        Node->getQualifierLoc().getNestedNameSpecifier();
    OverloadedOperatorKind OOK =
        Node->getNameInfo().getName().getCXXOverloadedOperator();
    if (QualifierLoc == nullptr && OOK != OO_None) {
      // Print reduction identifier in C format
      OS << getOperatorSpelling(OOK);
    } else {
      // Use C++ format
      if (QualifierLoc != nullptr)
        QualifierLoc->print(OS, Policy);
      OS << Node->getNameInfo();
    }
    OS << ":";
    VisitOMPClauseList(Node, ' ');
    OS << ")";
  }
}

void OMPClausePrinter::VisitOMPInReductionClause(OMPInReductionClause *Node) {
  if (!Node->varlist_empty()) {
    OS << "in_reduction(";
    NestedNameSpecifier *QualifierLoc =
        Node->getQualifierLoc().getNestedNameSpecifier();
    OverloadedOperatorKind OOK =
        Node->getNameInfo().getName().getCXXOverloadedOperator();
    if (QualifierLoc == nullptr && OOK != OO_None) {
      // Print reduction identifier in C format
      OS << getOperatorSpelling(OOK);
    } else {
      // Use C++ format
      if (QualifierLoc != nullptr)
        QualifierLoc->print(OS, Policy);
      OS << Node->getNameInfo();
    }
    OS << ":";
    VisitOMPClauseList(Node, ' ');
    OS << ")";
  }
}

void OMPClausePrinter::VisitOMPLinearClause(OMPLinearClause *Node) {
  if (!Node->varlist_empty()) {
    OS << "linear";
    if (Node->getModifierLoc().isValid()) {
      OS << '('
         << getOpenMPSimpleClauseTypeName(OMPC_linear, Node->getModifier());
    }
    VisitOMPClauseList(Node, '(');
    if (Node->getModifierLoc().isValid())
      OS << ')';
    if (Node->getStep() != nullptr) {
      OS << ": ";
      Node->getStep()->printPretty(OS, nullptr, Policy, 0);
    }
    OS << ")";
  }
}

void OMPClausePrinter::VisitOMPAlignedClause(OMPAlignedClause *Node) {
  if (!Node->varlist_empty()) {
    OS << "aligned";
    VisitOMPClauseList(Node, '(');
    if (Node->getAlignment() != nullptr) {
      OS << ": ";
      Node->getAlignment()->printPretty(OS, nullptr, Policy, 0);
    }
    OS << ")";
  }
}

void OMPClausePrinter::VisitOMPCopyinClause(OMPCopyinClause *Node) {
  if (!Node->varlist_empty()) {
    OS << "copyin";
    VisitOMPClauseList(Node, '(');
    OS << ")";
  }
}

void OMPClausePrinter::VisitOMPCopyprivateClause(OMPCopyprivateClause *Node) {
  if (!Node->varlist_empty()) {
    OS << "copyprivate";
    VisitOMPClauseList(Node, '(');
    OS << ")";
  }
}

void OMPClausePrinter::VisitOMPFlushClause(OMPFlushClause *Node) {
  if (!Node->varlist_empty()) {
    VisitOMPClauseList(Node, '(');
    OS << ")";
  }
}

void OMPClausePrinter::VisitOMPDependClause(OMPDependClause *Node) {
  OS << "depend(";
  OS << getOpenMPSimpleClauseTypeName(Node->getClauseKind(),
                                      Node->getDependencyKind());
  if (!Node->varlist_empty()) {
    OS << " :";
    VisitOMPClauseList(Node, ' ');
  }
  OS << ")";
}

void OMPClausePrinter::VisitOMPMapClause(OMPMapClause *Node) {
  if (!Node->varlist_empty()) {
    OS << "map(";
    if (Node->getMapType() != OMPC_MAP_unknown) {
      for (unsigned I = 0; I < OMPMapClause::NumberOfModifiers; ++I) {
        if (Node->getMapTypeModifier(I) != OMPC_MAP_MODIFIER_unknown) {
          OS << getOpenMPSimpleClauseTypeName(OMPC_map,
                                              Node->getMapTypeModifier(I));
          if (Node->getMapTypeModifier(I) == OMPC_MAP_MODIFIER_mapper) {
            OS << '(';
            NestedNameSpecifier *MapperNNS =
                Node->getMapperQualifierLoc().getNestedNameSpecifier();
            if (MapperNNS)
              MapperNNS->print(OS, Policy);
            OS << Node->getMapperIdInfo() << ')';
          }
          OS << ',';
        }
      }
      OS << getOpenMPSimpleClauseTypeName(OMPC_map, Node->getMapType());
      OS << ':';
    }
    VisitOMPClauseList(Node, ' ');
    OS << ")";
  }
}

void OMPClausePrinter::VisitOMPToClause(OMPToClause *Node) {
  if (!Node->varlist_empty()) {
    OS << "to";
    DeclarationNameInfo MapperId = Node->getMapperIdInfo();
    if (MapperId.getName() && !MapperId.getName().isEmpty()) {
      OS << '(';
      OS << "mapper(";
      NestedNameSpecifier *MapperNNS =
          Node->getMapperQualifierLoc().getNestedNameSpecifier();
      if (MapperNNS)
        MapperNNS->print(OS, Policy);
      OS << MapperId << "):";
      VisitOMPClauseList(Node, ' ');
    } else {
      VisitOMPClauseList(Node, '(');
    }
    OS << ")";
  }
}

void OMPClausePrinter::VisitOMPFromClause(OMPFromClause *Node) {
  if (!Node->varlist_empty()) {
    OS << "from";
    DeclarationNameInfo MapperId = Node->getMapperIdInfo();
    if (MapperId.getName() && !MapperId.getName().isEmpty()) {
      OS << '(';
      OS << "mapper(";
      NestedNameSpecifier *MapperNNS =
          Node->getMapperQualifierLoc().getNestedNameSpecifier();
      if (MapperNNS)
        MapperNNS->print(OS, Policy);
      OS << MapperId << "):";
      VisitOMPClauseList(Node, ' ');
    } else {
      VisitOMPClauseList(Node, '(');
    }
    OS << ")";
  }
}

void OMPClausePrinter::VisitOMPDistScheduleClause(OMPDistScheduleClause *Node) {
  OS << "dist_schedule(" << getOpenMPSimpleClauseTypeName(
                           OMPC_dist_schedule, Node->getDistScheduleKind());
  if (auto *E = Node->getChunkSize()) {
    OS << ", ";
    E->printPretty(OS, nullptr, Policy);
  }
  OS << ")";
}

void OMPClausePrinter::VisitOMPDefaultmapClause(OMPDefaultmapClause *Node) {
  OS << "defaultmap(";
  OS << getOpenMPSimpleClauseTypeName(OMPC_defaultmap,
                                      Node->getDefaultmapModifier());
  OS << ": ";
  OS << getOpenMPSimpleClauseTypeName(OMPC_defaultmap,
    Node->getDefaultmapKind());
  OS << ")";
}

void OMPClausePrinter::VisitOMPUseDevicePtrClause(OMPUseDevicePtrClause *Node) {
  if (!Node->varlist_empty()) {
    OS << "use_device_ptr";
    VisitOMPClauseList(Node, '(');
    OS << ")";
  }
}

void OMPClausePrinter::VisitOMPIsDevicePtrClause(OMPIsDevicePtrClause *Node) {
  if (!Node->varlist_empty()) {
    OS << "is_device_ptr";
    VisitOMPClauseList(Node, '(');
    OS << ")";
  }
}

