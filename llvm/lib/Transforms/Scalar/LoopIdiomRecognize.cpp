//===- LoopIdiomRecognize.cpp - Loop idiom recognition --------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This pass implements an idiom recognizer that transforms simple loops into a
// non-loop form.  In cases that this kicks in, it can be a significant
// performance win.
//
// If compiling for code size we avoid idiom recognition if the resulting
// code could be larger than the code for the original loop. One way this could
// happen is if the loop is not removable after idiom recognition due to the
// presence of non-idiom instructions. The initial implementation of the
// heuristics applies to idioms in multi-block loops.
//
//===----------------------------------------------------------------------===//
//
// TODO List:
//
// Future loop memory idioms to recognize:
//   memcmp, memmove, strlen, etc.
// Future floating point idioms to recognize in -ffast-math mode:
//   fpowi
// Future integer operation idioms to recognize:
//   ctpop
//
// Beware that isel's default lowering for ctpop is highly inefficient for
// i64 and larger types when i64 is legal and the value has few bits set.  It
// would be good to enhance isel to emit a loop for ctpop in this case.
//
// This could recognize common matrix multiplies and dot product idioms and
// replace them with calls to BLAS (if linked in??).
//
//===----------------------------------------------------------------------===//

#include "llvm/Transforms/Scalar/LoopIdiomRecognize.h"
#include "llvm/ADT/APInt.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/MapVector.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SetVector.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Analysis/AliasAnalysis.h"
#include "llvm/Analysis/LoopAccessAnalysis.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Analysis/LoopPass.h"
#include "llvm/Analysis/MemoryLocation.h"
#include "llvm/Analysis/OptimizationRemarkEmitter.h"
#include "llvm/Analysis/ScalarEvolution.h"
#include "llvm/Analysis/ScalarEvolutionExpander.h"
#include "llvm/Analysis/ScalarEvolutionExpressions.h"
#include "llvm/Analysis/TargetLibraryInfo.h"
#include "llvm/Analysis/TargetTransformInfo.h"
#include "llvm/Analysis/ValueTracking.h"
#include "llvm/IR/Attributes.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Constant.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/DebugLoc.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/GlobalValue.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InstrTypes.h"
#include "llvm/IR/Instruction.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/PassManager.h"
#include "llvm/IR/PatternMatch.h"
#include "llvm/IR/Type.h"
#include "llvm/IR/User.h"
#include "llvm/IR/Value.h"
#include "llvm/IR/ValueHandle.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Pass.h"
#include "llvm/Support/Casting.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/Scalar.h"
#include "llvm/Transforms/Scalar/LoopPassManager.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"
#include "llvm/Transforms/Utils/BuildLibCalls.h"
#include "llvm/Transforms/Utils/Local.h"
#include "llvm/Transforms/Utils/LoopUtils.h"
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <utility>
#include <vector>

using namespace llvm;

#define DEBUG_TYPE "loop-idiom"

STATISTIC(NumMemSet, "Number of memset's formed from loop stores");
STATISTIC(NumMemCpy, "Number of memcpy's formed from loop load+stores");
STATISTIC(NumBCmp, "Number of memcmp's formed from loop 2xload+eq-compare");

static cl::opt<bool> UseLIRCodeSizeHeurs(
    "use-lir-code-size-heurs",
    cl::desc("Use loop idiom recognition code size heuristics when compiling"
             "with -Os/-Oz"),
    cl::init(true), cl::Hidden);

namespace {

// FIXME: reinventing the wheel much? Is there a cleaner solution?
struct PMAbstraction {
  virtual void markLoopAsDeleted(Loop *L) = 0;
  virtual ~PMAbstraction() = default;
};
struct LegacyPMAbstraction : PMAbstraction {
  LPPassManager &LPM;
  LegacyPMAbstraction(LPPassManager &LPM) : LPM(LPM) {}
  virtual ~LegacyPMAbstraction() = default;
  void markLoopAsDeleted(Loop *L) override { LPM.markLoopAsDeleted(*L); }
};
struct NewPMAbstraction : PMAbstraction {
  LPMUpdater &Updater;
  NewPMAbstraction(LPMUpdater &Updater) : Updater(Updater) {}
  virtual ~NewPMAbstraction() = default;
  void markLoopAsDeleted(Loop *L) override {
    Updater.markLoopAsDeleted(*L, L->getName());
  }
};

class LoopIdiomRecognize {
  Loop *CurLoop = nullptr;
  AliasAnalysis *AA;
  DominatorTree *DT;
  LoopInfo *LI;
  ScalarEvolution *SE;
  TargetLibraryInfo *TLI;
  const TargetTransformInfo *TTI;
  const DataLayout *DL;
  PMAbstraction &LoopDeleter;
  OptimizationRemarkEmitter &ORE;
  bool ApplyCodeSizeHeuristics;

public:
  explicit LoopIdiomRecognize(AliasAnalysis *AA, DominatorTree *DT,
                              LoopInfo *LI, ScalarEvolution *SE,
                              TargetLibraryInfo *TLI,
                              const TargetTransformInfo *TTI,
                              const DataLayout *DL, PMAbstraction &LoopDeleter,
                              OptimizationRemarkEmitter &ORE)
      : AA(AA), DT(DT), LI(LI), SE(SE), TLI(TLI), TTI(TTI), DL(DL),
        LoopDeleter(LoopDeleter), ORE(ORE) {}

  bool runOnLoop(Loop *L);

private:
  using StoreList = SmallVector<StoreInst *, 8>;
  using StoreListMap = MapVector<Value *, StoreList>;

  StoreListMap StoreRefsForMemset;
  StoreListMap StoreRefsForMemsetPattern;
  StoreList StoreRefsForMemcpy;
  bool HasMemset;
  bool HasMemsetPattern;
  bool HasMemcpy;
  bool HasMemCmp;
  bool HasBCmp;

  /// Return code for isLegalStore()
  enum LegalStoreKind {
    None = 0,
    Memset,
    MemsetPattern,
    Memcpy,
    UnorderedAtomicMemcpy,
    DontUse // Dummy retval never to be used. Allows catching errors in retval
            // handling.
  };

  /// \name Countable Loop Idiom Handling
  /// @{

  bool runOnCountableLoop();
  bool runOnLoopBlock(BasicBlock *BB, const SCEV *BECount,
                      SmallVectorImpl<BasicBlock *> &ExitBlocks);

  void collectStores(BasicBlock *BB);
  LegalStoreKind isLegalStore(StoreInst *SI);
  enum class ForMemset { No, Yes };
  bool processLoopStores(SmallVectorImpl<StoreInst *> &SL, const SCEV *BECount,
                         ForMemset For);
  bool processLoopMemSet(MemSetInst *MSI, const SCEV *BECount);

  bool processLoopStridedStore(Value *DestPtr, unsigned StoreSize,
                               unsigned StoreAlignment, Value *StoredVal,
                               Instruction *TheStore,
                               SmallPtrSetImpl<Instruction *> &Stores,
                               const SCEVAddRecExpr *Ev, const SCEV *BECount,
                               bool NegStride, bool IsLoopMemset = false);
  bool processLoopStoreOfLoopLoad(StoreInst *SI, const SCEV *BECount);
  bool avoidLIRForMultiBlockLoop(bool IsMemset = false,
                                 bool IsLoopMemset = false);

  /// @}
  /// \name Noncountable Loop Idiom Handling
  /// @{

  bool runOnNoncountableLoop();

  struct CmpLoopStructure {
    Value *BCmpValue, *LatchCmpValue;
    BasicBlock *HeaderBrEqualBB, *HeaderBrUnequalBB;
    BasicBlock *LatchBrFinishBB, *LatchBrContinueBB;
  };
  bool matchBCmpLoopStructure(CmpLoopStructure &CmpLoop) const;
  struct CmpOfLoads {
    ICmpInst::Predicate BCmpPred;
    Value *LoadSrcA, *LoadSrcB;
    Value *LoadA, *LoadB;
  };
  bool matchBCmpOfLoads(Value *BCmpValue, CmpOfLoads &CmpOfLoads) const;
  bool recognizeBCmpLoopControlFlow(const CmpOfLoads &CmpOfLoads,
                                    CmpLoopStructure &CmpLoop) const;
  bool recognizeBCmpLoopSCEV(uint64_t BCmpTyBytes, CmpOfLoads &CmpOfLoads,
                             const SCEV *&SrcA, const SCEV *&SrcB,
                             const SCEV *&Iterations) const;
  bool detectBCmpIdiom(ICmpInst *&BCmpInst, CmpInst *&LatchCmpInst,
                       LoadInst *&LoadA, LoadInst *&LoadB, const SCEV *&SrcA,
                       const SCEV *&SrcB, const SCEV *&NBytes) const;
  BasicBlock *transformBCmpControlFlow(ICmpInst *ComparedEqual);
  void transformLoopToBCmp(ICmpInst *BCmpInst, CmpInst *LatchCmpInst,
                           LoadInst *LoadA, LoadInst *LoadB, const SCEV *SrcA,
                           const SCEV *SrcB, const SCEV *NBytes);
  bool recognizeBCmp();

  bool recognizePopcount();
  void transformLoopToPopcount(BasicBlock *PreCondBB, Instruction *CntInst,
                               PHINode *CntPhi, Value *Var);
  bool recognizeAndInsertFFS();  /// Find First Set: ctlz or cttz
  void transformLoopToCountable(Intrinsic::ID IntrinID, BasicBlock *PreCondBB,
                                Instruction *CntInst, PHINode *CntPhi,
                                Value *Var, Instruction *DefX,
                                const DebugLoc &DL, bool ZeroCheck,
                                bool IsCntPhiUsedOutsideLoop);

  /// @}
};

class LoopIdiomRecognizeLegacyPass : public LoopPass {
public:
  static char ID;

  explicit LoopIdiomRecognizeLegacyPass() : LoopPass(ID) {
    initializeLoopIdiomRecognizeLegacyPassPass(
        *PassRegistry::getPassRegistry());
  }

  bool runOnLoop(Loop *L, LPPassManager &LPM) override {
    if (skipLoop(L))
      return false;

    AliasAnalysis *AA = &getAnalysis<AAResultsWrapperPass>().getAAResults();
    DominatorTree *DT = &getAnalysis<DominatorTreeWrapperPass>().getDomTree();
    LoopInfo *LI = &getAnalysis<LoopInfoWrapperPass>().getLoopInfo();
    ScalarEvolution *SE = &getAnalysis<ScalarEvolutionWrapperPass>().getSE();
    TargetLibraryInfo *TLI =
        &getAnalysis<TargetLibraryInfoWrapperPass>().getTLI(
            *L->getHeader()->getParent());
    const TargetTransformInfo *TTI =
        &getAnalysis<TargetTransformInfoWrapperPass>().getTTI(
            *L->getHeader()->getParent());
    const DataLayout *DL = &L->getHeader()->getModule()->getDataLayout();
    LegacyPMAbstraction LoopDeleter(LPM);

    // For the old PM, we can't use OptimizationRemarkEmitter as an analysis
    // pass.  Function analyses need to be preserved across loop transformations
    // but ORE cannot be preserved (see comment before the pass definition).
    OptimizationRemarkEmitter ORE(L->getHeader()->getParent());

    LoopIdiomRecognize LIR(AA, DT, LI, SE, TLI, TTI, DL, LoopDeleter, ORE);
    return LIR.runOnLoop(L);
  }

  /// This transformation requires natural loop information & requires that
  /// loop preheaders be inserted into the CFG.
  void getAnalysisUsage(AnalysisUsage &AU) const override {
    AU.addRequired<TargetLibraryInfoWrapperPass>();
    AU.addRequired<TargetTransformInfoWrapperPass>();
    getLoopAnalysisUsage(AU);
  }
};

} // end anonymous namespace

char LoopIdiomRecognizeLegacyPass::ID = 0;

PreservedAnalyses LoopIdiomRecognizePass::run(Loop &L, LoopAnalysisManager &AM,
                                              LoopStandardAnalysisResults &AR,
                                              LPMUpdater &Updater) {
  const auto *DL = &L.getHeader()->getModule()->getDataLayout();

  const auto &FAM =
      AM.getResult<FunctionAnalysisManagerLoopProxy>(L, AR).getManager();
  Function *F = L.getHeader()->getParent();

  auto *ORE = FAM.getCachedResult<OptimizationRemarkEmitterAnalysis>(*F);
  // FIXME: This should probably be optional rather than required.
  if (!ORE)
    report_fatal_error(
        "LoopIdiomRecognizePass: OptimizationRemarkEmitterAnalysis not cached "
        "at a higher level");

  NewPMAbstraction LoopDeleter(Updater);
  LoopIdiomRecognize LIR(&AR.AA, &AR.DT, &AR.LI, &AR.SE, &AR.TLI, &AR.TTI, DL,
                         LoopDeleter, *ORE);
  if (!LIR.runOnLoop(&L))
    return PreservedAnalyses::all();

  return getLoopPassPreservedAnalyses();
}

INITIALIZE_PASS_BEGIN(LoopIdiomRecognizeLegacyPass, "loop-idiom",
                      "Recognize loop idioms", false, false)
INITIALIZE_PASS_DEPENDENCY(LoopPass)
INITIALIZE_PASS_DEPENDENCY(TargetLibraryInfoWrapperPass)
INITIALIZE_PASS_DEPENDENCY(TargetTransformInfoWrapperPass)
INITIALIZE_PASS_END(LoopIdiomRecognizeLegacyPass, "loop-idiom",
                    "Recognize loop idioms", false, false)

Pass *llvm::createLoopIdiomPass() { return new LoopIdiomRecognizeLegacyPass(); }

static void deleteDeadInstruction(Instruction *I) {
  I->replaceAllUsesWith(UndefValue::get(I->getType()));
  I->eraseFromParent();
}

//===----------------------------------------------------------------------===//
//
//          Implementation of LoopIdiomRecognize
//
//===----------------------------------------------------------------------===//

bool LoopIdiomRecognize::runOnLoop(Loop *L) {
  CurLoop = L;
  // If the loop could not be converted to canonical form, it must have an
  // indirectbr in it, just give up.
  if (!L->getLoopPreheader())
    return false;

  // Disable loop idiom recognition if the function's name is a common idiom.
  StringRef Name = L->getHeader()->getParent()->getName();
  if (Name == "memset" || Name == "memcpy" || Name == "memcmp" ||
      Name == "bcmp")
    return false;

  // Determine if code size heuristics need to be applied.
  ApplyCodeSizeHeuristics =
      L->getHeader()->getParent()->hasOptSize() && UseLIRCodeSizeHeurs;

  HasMemset = TLI->has(LibFunc_memset);
  HasMemsetPattern = TLI->has(LibFunc_memset_pattern16);
  HasMemcpy = TLI->has(LibFunc_memcpy);
  HasMemCmp = TLI->has(LibFunc_memcmp);
  HasBCmp = TLI->has(LibFunc_bcmp);

  if (HasMemset || HasMemsetPattern || HasMemcpy || HasMemCmp || HasBCmp)
    if (SE->hasLoopInvariantBackedgeTakenCount(L))
      return runOnCountableLoop();

  return runOnNoncountableLoop();
}

bool LoopIdiomRecognize::runOnCountableLoop() {
  const SCEV *BECount = SE->getBackedgeTakenCount(CurLoop);
  assert(!isa<SCEVCouldNotCompute>(BECount) &&
         "runOnCountableLoop() called on a loop without a predictable"
         "backedge-taken count");

  // If this loop executes exactly one time, then it should be peeled, not
  // optimized by this pass.
  if (const SCEVConstant *BECst = dyn_cast<SCEVConstant>(BECount))
    if (BECst->getAPInt() == 0)
      return false;

  SmallVector<BasicBlock *, 8> ExitBlocks;
  CurLoop->getUniqueExitBlocks(ExitBlocks);

  LLVM_DEBUG(dbgs() << DEBUG_TYPE " Scanning: F["
                    << CurLoop->getHeader()->getParent()->getName()
                    << "] Countable Loop %" << CurLoop->getHeader()->getName()
                    << "\n");

  bool MadeChange = false;

  // The following transforms hoist stores/memsets into the loop pre-header.
  // Give up if the loop has instructions may throw.
  SimpleLoopSafetyInfo SafetyInfo;
  SafetyInfo.computeLoopSafetyInfo(CurLoop);
  if (SafetyInfo.anyBlockMayThrow())
    return MadeChange;

  // Scan all the blocks in the loop that are not in subloops.
  for (auto *BB : CurLoop->getBlocks()) {
    // Ignore blocks in subloops.
    if (LI->getLoopFor(BB) != CurLoop)
      continue;

    MadeChange |= runOnLoopBlock(BB, BECount, ExitBlocks);
  }
  return MadeChange;
}

static APInt getStoreStride(const SCEVAddRecExpr *StoreEv) {
  const SCEVConstant *ConstStride = cast<SCEVConstant>(StoreEv->getOperand(1));
  return ConstStride->getAPInt();
}

/// getMemSetPatternValue - If a strided store of the specified value is safe to
/// turn into a memset_pattern16, return a ConstantArray of 16 bytes that should
/// be passed in.  Otherwise, return null.
///
/// Note that we don't ever attempt to use memset_pattern8 or 4, because these
/// just replicate their input array and then pass on to memset_pattern16.
static Constant *getMemSetPatternValue(Value *V, const DataLayout *DL) {
  // FIXME: This could check for UndefValue because it can be merged into any
  // other valid pattern.

  // If the value isn't a constant, we can't promote it to being in a constant
  // array.  We could theoretically do a store to an alloca or something, but
  // that doesn't seem worthwhile.
  Constant *C = dyn_cast<Constant>(V);
  if (!C)
    return nullptr;

  // Only handle simple values that are a power of two bytes in size.
  uint64_t Size = DL->getTypeSizeInBits(V->getType());
  if (Size == 0 || (Size & 7) || (Size & (Size - 1)))
    return nullptr;

  // Don't care enough about darwin/ppc to implement this.
  if (DL->isBigEndian())
    return nullptr;

  // Convert to size in bytes.
  Size /= 8;

  // TODO: If CI is larger than 16-bytes, we can try slicing it in half to see
  // if the top and bottom are the same (e.g. for vectors and large integers).
  if (Size > 16)
    return nullptr;

  // If the constant is exactly 16 bytes, just use it.
  if (Size == 16)
    return C;

  // Otherwise, we'll use an array of the constants.
  unsigned ArraySize = 16 / Size;
  ArrayType *AT = ArrayType::get(V->getType(), ArraySize);
  return ConstantArray::get(AT, std::vector<Constant *>(ArraySize, C));
}

LoopIdiomRecognize::LegalStoreKind
LoopIdiomRecognize::isLegalStore(StoreInst *SI) {
  // Don't touch volatile stores.
  if (SI->isVolatile())
    return LegalStoreKind::None;
  // We only want simple or unordered-atomic stores.
  if (!SI->isUnordered())
    return LegalStoreKind::None;

  // Don't convert stores of non-integral pointer types to memsets (which stores
  // integers).
  if (DL->isNonIntegralPointerType(SI->getValueOperand()->getType()))
    return LegalStoreKind::None;

  // Avoid merging nontemporal stores.
  if (SI->getMetadata(LLVMContext::MD_nontemporal))
    return LegalStoreKind::None;

  Value *StoredVal = SI->getValueOperand();
  Value *StorePtr = SI->getPointerOperand();

  // Reject stores that are so large that they overflow an unsigned.
  uint64_t SizeInBits = DL->getTypeSizeInBits(StoredVal->getType());
  if ((SizeInBits & 7) || (SizeInBits >> 32) != 0)
    return LegalStoreKind::None;

  // See if the pointer expression is an AddRec like {base,+,1} on the current
  // loop, which indicates a strided store.  If we have something else, it's a
  // random store we can't handle.
  const SCEVAddRecExpr *StoreEv =
      dyn_cast<SCEVAddRecExpr>(SE->getSCEV(StorePtr));
  if (!StoreEv || StoreEv->getLoop() != CurLoop || !StoreEv->isAffine())
    return LegalStoreKind::None;

  // Check to see if we have a constant stride.
  if (!isa<SCEVConstant>(StoreEv->getOperand(1)))
    return LegalStoreKind::None;

  // See if the store can be turned into a memset.

  // If the stored value is a byte-wise value (like i32 -1), then it may be
  // turned into a memset of i8 -1, assuming that all the consecutive bytes
  // are stored.  A store of i32 0x01020304 can never be turned into a memset,
  // but it can be turned into memset_pattern if the target supports it.
  Value *SplatValue = isBytewiseValue(StoredVal, *DL);
  Constant *PatternValue = nullptr;

  // Note: memset and memset_pattern on unordered-atomic is yet not supported
  bool UnorderedAtomic = SI->isUnordered() && !SI->isSimple();

  // If we're allowed to form a memset, and the stored value would be
  // acceptable for memset, use it.
  if (!UnorderedAtomic && HasMemset && SplatValue &&
      // Verify that the stored value is loop invariant.  If not, we can't
      // promote the memset.
      CurLoop->isLoopInvariant(SplatValue)) {
    // It looks like we can use SplatValue.
    return LegalStoreKind::Memset;
  } else if (!UnorderedAtomic && HasMemsetPattern &&
             // Don't create memset_pattern16s with address spaces.
             StorePtr->getType()->getPointerAddressSpace() == 0 &&
             (PatternValue = getMemSetPatternValue(StoredVal, DL))) {
    // It looks like we can use PatternValue!
    return LegalStoreKind::MemsetPattern;
  }

  // Otherwise, see if the store can be turned into a memcpy.
  if (HasMemcpy) {
    // Check to see if the stride matches the size of the store.  If so, then we
    // know that every byte is touched in the loop.
    APInt Stride = getStoreStride(StoreEv);
    unsigned StoreSize = DL->getTypeStoreSize(SI->getValueOperand()->getType());
    if (StoreSize != Stride && StoreSize != -Stride)
      return LegalStoreKind::None;

    // The store must be feeding a non-volatile load.
    LoadInst *LI = dyn_cast<LoadInst>(SI->getValueOperand());

    // Only allow non-volatile loads
    if (!LI || LI->isVolatile())
      return LegalStoreKind::None;
    // Only allow simple or unordered-atomic loads
    if (!LI->isUnordered())
      return LegalStoreKind::None;

    // See if the pointer expression is an AddRec like {base,+,1} on the current
    // loop, which indicates a strided load.  If we have something else, it's a
    // random load we can't handle.
    const SCEVAddRecExpr *LoadEv =
        dyn_cast<SCEVAddRecExpr>(SE->getSCEV(LI->getPointerOperand()));
    if (!LoadEv || LoadEv->getLoop() != CurLoop || !LoadEv->isAffine())
      return LegalStoreKind::None;

    // The store and load must share the same stride.
    if (StoreEv->getOperand(1) != LoadEv->getOperand(1))
      return LegalStoreKind::None;

    // Success.  This store can be converted into a memcpy.
    UnorderedAtomic = UnorderedAtomic || LI->isAtomic();
    return UnorderedAtomic ? LegalStoreKind::UnorderedAtomicMemcpy
                           : LegalStoreKind::Memcpy;
  }
  // This store can't be transformed into a memset/memcpy.
  return LegalStoreKind::None;
}

void LoopIdiomRecognize::collectStores(BasicBlock *BB) {
  StoreRefsForMemset.clear();
  StoreRefsForMemsetPattern.clear();
  StoreRefsForMemcpy.clear();
  for (Instruction &I : *BB) {
    StoreInst *SI = dyn_cast<StoreInst>(&I);
    if (!SI)
      continue;

    // Make sure this is a strided store with a constant stride.
    switch (isLegalStore(SI)) {
    case LegalStoreKind::None:
      // Nothing to do
      break;
    case LegalStoreKind::Memset: {
      // Find the base pointer.
      Value *Ptr = GetUnderlyingObject(SI->getPointerOperand(), *DL);
      StoreRefsForMemset[Ptr].push_back(SI);
    } break;
    case LegalStoreKind::MemsetPattern: {
      // Find the base pointer.
      Value *Ptr = GetUnderlyingObject(SI->getPointerOperand(), *DL);
      StoreRefsForMemsetPattern[Ptr].push_back(SI);
    } break;
    case LegalStoreKind::Memcpy:
    case LegalStoreKind::UnorderedAtomicMemcpy:
      StoreRefsForMemcpy.push_back(SI);
      break;
    default:
      assert(false && "unhandled return value");
      break;
    }
  }
}

/// runOnLoopBlock - Process the specified block, which lives in a counted loop
/// with the specified backedge count.  This block is known to be in the current
/// loop and not in any subloops.
bool LoopIdiomRecognize::runOnLoopBlock(
    BasicBlock *BB, const SCEV *BECount,
    SmallVectorImpl<BasicBlock *> &ExitBlocks) {
  // We can only promote stores in this block if they are unconditionally
  // executed in the loop.  For a block to be unconditionally executed, it has
  // to dominate all the exit blocks of the loop.  Verify this now.
  for (unsigned i = 0, e = ExitBlocks.size(); i != e; ++i)
    if (!DT->dominates(BB, ExitBlocks[i]))
      return false;

  bool MadeChange = false;
  // Look for store instructions, which may be optimized to memset/memcpy.
  collectStores(BB);

  // Look for a single store or sets of stores with a common base, which can be
  // optimized into a memset (memset_pattern).  The latter most commonly happens
  // with structs and handunrolled loops.
  for (auto &SL : StoreRefsForMemset)
    MadeChange |= processLoopStores(SL.second, BECount, ForMemset::Yes);

  for (auto &SL : StoreRefsForMemsetPattern)
    MadeChange |= processLoopStores(SL.second, BECount, ForMemset::No);

  // Optimize the store into a memcpy, if it feeds an similarly strided load.
  for (auto &SI : StoreRefsForMemcpy)
    MadeChange |= processLoopStoreOfLoopLoad(SI, BECount);

  for (BasicBlock::iterator I = BB->begin(), E = BB->end(); I != E;) {
    Instruction *Inst = &*I++;
    // Look for memset instructions, which may be optimized to a larger memset.
    if (MemSetInst *MSI = dyn_cast<MemSetInst>(Inst)) {
      WeakTrackingVH InstPtr(&*I);
      if (!processLoopMemSet(MSI, BECount))
        continue;
      MadeChange = true;

      // If processing the memset invalidated our iterator, start over from the
      // top of the block.
      if (!InstPtr)
        I = BB->begin();
      continue;
    }
  }

  return MadeChange;
}

/// See if this store(s) can be promoted to a memset.
bool LoopIdiomRecognize::processLoopStores(SmallVectorImpl<StoreInst *> &SL,
                                           const SCEV *BECount, ForMemset For) {
  // Try to find consecutive stores that can be transformed into memsets.
  SetVector<StoreInst *> Heads, Tails;
  SmallDenseMap<StoreInst *, StoreInst *> ConsecutiveChain;

  // Do a quadratic search on all of the given stores and find
  // all of the pairs of stores that follow each other.
  SmallVector<unsigned, 16> IndexQueue;
  for (unsigned i = 0, e = SL.size(); i < e; ++i) {
    assert(SL[i]->isSimple() && "Expected only non-volatile stores.");

    Value *FirstStoredVal = SL[i]->getValueOperand();
    Value *FirstStorePtr = SL[i]->getPointerOperand();
    const SCEVAddRecExpr *FirstStoreEv =
        cast<SCEVAddRecExpr>(SE->getSCEV(FirstStorePtr));
    APInt FirstStride = getStoreStride(FirstStoreEv);
    unsigned FirstStoreSize = DL->getTypeStoreSize(SL[i]->getValueOperand()->getType());

    // See if we can optimize just this store in isolation.
    if (FirstStride == FirstStoreSize || -FirstStride == FirstStoreSize) {
      Heads.insert(SL[i]);
      continue;
    }

    Value *FirstSplatValue = nullptr;
    Constant *FirstPatternValue = nullptr;

    if (For == ForMemset::Yes)
      FirstSplatValue = isBytewiseValue(FirstStoredVal, *DL);
    else
      FirstPatternValue = getMemSetPatternValue(FirstStoredVal, DL);

    assert((FirstSplatValue || FirstPatternValue) &&
           "Expected either splat value or pattern value.");

    IndexQueue.clear();
    // If a store has multiple consecutive store candidates, search Stores
    // array according to the sequence: from i+1 to e, then from i-1 to 0.
    // This is because usually pairing with immediate succeeding or preceding
    // candidate create the best chance to find memset opportunity.
    unsigned j = 0;
    for (j = i + 1; j < e; ++j)
      IndexQueue.push_back(j);
    for (j = i; j > 0; --j)
      IndexQueue.push_back(j - 1);

    for (auto &k : IndexQueue) {
      assert(SL[k]->isSimple() && "Expected only non-volatile stores.");
      Value *SecondStorePtr = SL[k]->getPointerOperand();
      const SCEVAddRecExpr *SecondStoreEv =
          cast<SCEVAddRecExpr>(SE->getSCEV(SecondStorePtr));
      APInt SecondStride = getStoreStride(SecondStoreEv);

      if (FirstStride != SecondStride)
        continue;

      Value *SecondStoredVal = SL[k]->getValueOperand();
      Value *SecondSplatValue = nullptr;
      Constant *SecondPatternValue = nullptr;

      if (For == ForMemset::Yes)
        SecondSplatValue = isBytewiseValue(SecondStoredVal, *DL);
      else
        SecondPatternValue = getMemSetPatternValue(SecondStoredVal, DL);

      assert((SecondSplatValue || SecondPatternValue) &&
             "Expected either splat value or pattern value.");

      if (isConsecutiveAccess(SL[i], SL[k], *DL, *SE, false)) {
        if (For == ForMemset::Yes) {
          if (isa<UndefValue>(FirstSplatValue))
            FirstSplatValue = SecondSplatValue;
          if (FirstSplatValue != SecondSplatValue)
            continue;
        } else {
          if (isa<UndefValue>(FirstPatternValue))
            FirstPatternValue = SecondPatternValue;
          if (FirstPatternValue != SecondPatternValue)
            continue;
        }
        Tails.insert(SL[k]);
        Heads.insert(SL[i]);
        ConsecutiveChain[SL[i]] = SL[k];
        break;
      }
    }
  }

  // We may run into multiple chains that merge into a single chain. We mark the
  // stores that we transformed so that we don't visit the same store twice.
  SmallPtrSet<Value *, 16> TransformedStores;
  bool Changed = false;

  // For stores that start but don't end a link in the chain:
  for (SetVector<StoreInst *>::iterator it = Heads.begin(), e = Heads.end();
       it != e; ++it) {
    if (Tails.count(*it))
      continue;

    // We found a store instr that starts a chain. Now follow the chain and try
    // to transform it.
    SmallPtrSet<Instruction *, 8> AdjacentStores;
    StoreInst *I = *it;

    StoreInst *HeadStore = I;
    unsigned StoreSize = 0;

    // Collect the chain into a list.
    while (Tails.count(I) || Heads.count(I)) {
      if (TransformedStores.count(I))
        break;
      AdjacentStores.insert(I);

      StoreSize += DL->getTypeStoreSize(I->getValueOperand()->getType());
      // Move to the next value in the chain.
      I = ConsecutiveChain[I];
    }

    Value *StoredVal = HeadStore->getValueOperand();
    Value *StorePtr = HeadStore->getPointerOperand();
    const SCEVAddRecExpr *StoreEv = cast<SCEVAddRecExpr>(SE->getSCEV(StorePtr));
    APInt Stride = getStoreStride(StoreEv);

    // Check to see if the stride matches the size of the stores.  If so, then
    // we know that every byte is touched in the loop.
    if (StoreSize != Stride && StoreSize != -Stride)
      continue;

    bool NegStride = StoreSize == -Stride;

    if (processLoopStridedStore(StorePtr, StoreSize, HeadStore->getAlignment(),
                                StoredVal, HeadStore, AdjacentStores, StoreEv,
                                BECount, NegStride)) {
      TransformedStores.insert(AdjacentStores.begin(), AdjacentStores.end());
      Changed = true;
    }
  }

  return Changed;
}

/// processLoopMemSet - See if this memset can be promoted to a large memset.
bool LoopIdiomRecognize::processLoopMemSet(MemSetInst *MSI,
                                           const SCEV *BECount) {
  // We can only handle non-volatile memsets with a constant size.
  if (MSI->isVolatile() || !isa<ConstantInt>(MSI->getLength()))
    return false;

  // If we're not allowed to hack on memset, we fail.
  if (!HasMemset)
    return false;

  Value *Pointer = MSI->getDest();

  // See if the pointer expression is an AddRec like {base,+,1} on the current
  // loop, which indicates a strided store.  If we have something else, it's a
  // random store we can't handle.
  const SCEVAddRecExpr *Ev = dyn_cast<SCEVAddRecExpr>(SE->getSCEV(Pointer));
  if (!Ev || Ev->getLoop() != CurLoop || !Ev->isAffine())
    return false;

  // Reject memsets that are so large that they overflow an unsigned.
  uint64_t SizeInBytes = cast<ConstantInt>(MSI->getLength())->getZExtValue();
  if ((SizeInBytes >> 32) != 0)
    return false;

  // Check to see if the stride matches the size of the memset.  If so, then we
  // know that every byte is touched in the loop.
  const SCEVConstant *ConstStride = dyn_cast<SCEVConstant>(Ev->getOperand(1));
  if (!ConstStride)
    return false;

  APInt Stride = ConstStride->getAPInt();
  if (SizeInBytes != Stride && SizeInBytes != -Stride)
    return false;

  // Verify that the memset value is loop invariant.  If not, we can't promote
  // the memset.
  Value *SplatValue = MSI->getValue();
  if (!SplatValue || !CurLoop->isLoopInvariant(SplatValue))
    return false;

  SmallPtrSet<Instruction *, 1> MSIs;
  MSIs.insert(MSI);
  bool NegStride = SizeInBytes == -Stride;
  return processLoopStridedStore(Pointer, (unsigned)SizeInBytes,
                                 MSI->getDestAlignment(), SplatValue, MSI, MSIs,
                                 Ev, BECount, NegStride, /*IsLoopMemset=*/true);
}

/// mayLoopAccessLocation - Return true if the specified loop might access the
/// specified pointer location, which is a loop-strided access.  The 'Access'
/// argument specifies what the verboten forms of access are (read or write).
static bool
mayLoopAccessLocation(Value *Ptr, ModRefInfo Access, Loop *L,
                      const SCEV *BECount, unsigned StoreSize,
                      AliasAnalysis &AA,
                      SmallPtrSetImpl<Instruction *> &IgnoredStores) {
  // Get the location that may be stored across the loop.  Since the access is
  // strided positively through memory, we say that the modified location starts
  // at the pointer and has infinite size.
  LocationSize AccessSize = LocationSize::unknown();

  // If the loop iterates a fixed number of times, we can refine the access size
  // to be exactly the size of the memset, which is (BECount+1)*StoreSize
  if (const SCEVConstant *BECst = dyn_cast<SCEVConstant>(BECount))
    AccessSize = LocationSize::precise((BECst->getValue()->getZExtValue() + 1) *
                                       StoreSize);

  // TODO: For this to be really effective, we have to dive into the pointer
  // operand in the store.  Store to &A[i] of 100 will always return may alias
  // with store of &A[100], we need to StoreLoc to be "A" with size of 100,
  // which will then no-alias a store to &A[100].
  MemoryLocation StoreLoc(Ptr, AccessSize);

  for (Loop::block_iterator BI = L->block_begin(), E = L->block_end(); BI != E;
       ++BI)
    for (Instruction &I : **BI)
      if (IgnoredStores.count(&I) == 0 &&
          isModOrRefSet(
              intersectModRef(AA.getModRefInfo(&I, StoreLoc), Access)))
        return true;

  return false;
}

// If we have a negative stride, Start refers to the end of the memory location
// we're trying to memset.  Therefore, we need to recompute the base pointer,
// which is just Start - BECount*Size.
static const SCEV *getStartForNegStride(const SCEV *Start, const SCEV *BECount,
                                        Type *IntPtr, unsigned StoreSize,
                                        ScalarEvolution *SE) {
  const SCEV *Index = SE->getTruncateOrZeroExtend(BECount, IntPtr);
  if (StoreSize != 1)
    Index = SE->getMulExpr(Index, SE->getConstant(IntPtr, StoreSize),
                           SCEV::FlagNUW);
  return SE->getMinusSCEV(Start, Index);
}

/// Compute the number of bytes as a SCEV from the backedge taken count.
///
/// This also maps the SCEV into the provided type and tries to handle the
/// computation in a way that will fold cleanly.
static const SCEV *getNumBytes(const SCEV *BECount, Type *IntPtr,
                               unsigned StoreSize, Loop *CurLoop,
                               const DataLayout *DL, ScalarEvolution *SE) {
  const SCEV *NumBytesS;
  // The # stored bytes is (BECount+1)*Size.  Expand the trip count out to
  // pointer size if it isn't already.
  //
  // If we're going to need to zero extend the BE count, check if we can add
  // one to it prior to zero extending without overflow. Provided this is safe,
  // it allows better simplification of the +1.
  if (DL->getTypeSizeInBits(BECount->getType()) <
          DL->getTypeSizeInBits(IntPtr) &&
      SE->isLoopEntryGuardedByCond(
          CurLoop, ICmpInst::ICMP_NE, BECount,
          SE->getNegativeSCEV(SE->getOne(BECount->getType())))) {
    NumBytesS = SE->getZeroExtendExpr(
        SE->getAddExpr(BECount, SE->getOne(BECount->getType()), SCEV::FlagNUW),
        IntPtr);
  } else {
    NumBytesS = SE->getAddExpr(SE->getTruncateOrZeroExtend(BECount, IntPtr),
                               SE->getOne(IntPtr), SCEV::FlagNUW);
  }

  // And scale it based on the store size.
  if (StoreSize != 1) {
    NumBytesS = SE->getMulExpr(NumBytesS, SE->getConstant(IntPtr, StoreSize),
                               SCEV::FlagNUW);
  }
  return NumBytesS;
}

/// processLoopStridedStore - We see a strided store of some value.  If we can
/// transform this into a memset or memset_pattern in the loop preheader, do so.
bool LoopIdiomRecognize::processLoopStridedStore(
    Value *DestPtr, unsigned StoreSize, unsigned StoreAlignment,
    Value *StoredVal, Instruction *TheStore,
    SmallPtrSetImpl<Instruction *> &Stores, const SCEVAddRecExpr *Ev,
    const SCEV *BECount, bool NegStride, bool IsLoopMemset) {
  Value *SplatValue = isBytewiseValue(StoredVal, *DL);
  Constant *PatternValue = nullptr;

  if (!SplatValue)
    PatternValue = getMemSetPatternValue(StoredVal, DL);

  assert((SplatValue || PatternValue) &&
         "Expected either splat value or pattern value.");

  // The trip count of the loop and the base pointer of the addrec SCEV is
  // guaranteed to be loop invariant, which means that it should dominate the
  // header.  This allows us to insert code for it in the preheader.
  unsigned DestAS = DestPtr->getType()->getPointerAddressSpace();
  BasicBlock *Preheader = CurLoop->getLoopPreheader();
  IRBuilder<> Builder(Preheader->getTerminator());
  SCEVExpander Expander(*SE, *DL, "loop-idiom");

  Type *DestInt8PtrTy = Builder.getInt8PtrTy(DestAS);
  Type *IntPtr = Builder.getIntPtrTy(*DL, DestAS);

  const SCEV *Start = Ev->getStart();
  // Handle negative strided loops.
  if (NegStride)
    Start = getStartForNegStride(Start, BECount, IntPtr, StoreSize, SE);

  // TODO: ideally we should still be able to generate memset if SCEV expander
  // is taught to generate the dependencies at the latest point.
  if (!isSafeToExpand(Start, *SE))
    return false;

  // Okay, we have a strided store "p[i]" of a splattable value.  We can turn
  // this into a memset in the loop preheader now if we want.  However, this
  // would be unsafe to do if there is anything else in the loop that may read
  // or write to the aliased location.  Check for any overlap by generating the
  // base pointer and checking the region.
  Value *BasePtr =
      Expander.expandCodeFor(Start, DestInt8PtrTy, Preheader->getTerminator());
  if (mayLoopAccessLocation(BasePtr, ModRefInfo::ModRef, CurLoop, BECount,
                            StoreSize, *AA, Stores)) {
    Expander.clear();
    // If we generated new code for the base pointer, clean up.
    RecursivelyDeleteTriviallyDeadInstructions(BasePtr, TLI);
    return false;
  }

  if (avoidLIRForMultiBlockLoop(/*IsMemset=*/true, IsLoopMemset))
    return false;

  // Okay, everything looks good, insert the memset.

  const SCEV *NumBytesS =
      getNumBytes(BECount, IntPtr, StoreSize, CurLoop, DL, SE);

  // TODO: ideally we should still be able to generate memset if SCEV expander
  // is taught to generate the dependencies at the latest point.
  if (!isSafeToExpand(NumBytesS, *SE))
    return false;

  Value *NumBytes =
      Expander.expandCodeFor(NumBytesS, IntPtr, Preheader->getTerminator());

  CallInst *NewCall;
  if (SplatValue) {
    NewCall =
        Builder.CreateMemSet(BasePtr, SplatValue, NumBytes, StoreAlignment);
  } else {
    // Everything is emitted in default address space
    Type *Int8PtrTy = DestInt8PtrTy;

    Module *M = TheStore->getModule();
    StringRef FuncName = "memset_pattern16";
    FunctionCallee MSP = M->getOrInsertFunction(FuncName, Builder.getVoidTy(),
                                                Int8PtrTy, Int8PtrTy, IntPtr);
    inferLibFuncAttributes(M, FuncName, *TLI);

    // Otherwise we should form a memset_pattern16.  PatternValue is known to be
    // an constant array of 16-bytes.  Plop the value into a mergable global.
    GlobalVariable *GV = new GlobalVariable(*M, PatternValue->getType(), true,
                                            GlobalValue::PrivateLinkage,
                                            PatternValue, ".memset_pattern");
    GV->setUnnamedAddr(GlobalValue::UnnamedAddr::Global); // Ok to merge these.
    GV->setAlignment(16);
    Value *PatternPtr = ConstantExpr::getBitCast(GV, Int8PtrTy);
    NewCall = Builder.CreateCall(MSP, {BasePtr, PatternPtr, NumBytes});
  }

  LLVM_DEBUG(dbgs() << "  Formed memset: " << *NewCall << "\n"
                    << "    from store to: " << *Ev << " at: " << *TheStore
                    << "\n");
  NewCall->setDebugLoc(TheStore->getDebugLoc());

  ORE.emit([&]() {
    return OptimizationRemark(DEBUG_TYPE, "ProcessLoopStridedStore",
                              NewCall->getDebugLoc(), Preheader)
           << "Transformed loop-strided store into a call to "
           << ore::NV("NewFunction", NewCall->getCalledFunction())
           << "() function";
  });

  // Okay, the memset has been formed.  Zap the original store and anything that
  // feeds into it.
  for (auto *I : Stores)
    deleteDeadInstruction(I);
  ++NumMemSet;
  return true;
}

/// If the stored value is a strided load in the same loop with the same stride
/// this may be transformable into a memcpy.  This kicks in for stuff like
/// for (i) A[i] = B[i];
bool LoopIdiomRecognize::processLoopStoreOfLoopLoad(StoreInst *SI,
                                                    const SCEV *BECount) {
  assert(SI->isUnordered() && "Expected only non-volatile non-ordered stores.");

  Value *StorePtr = SI->getPointerOperand();
  const SCEVAddRecExpr *StoreEv = cast<SCEVAddRecExpr>(SE->getSCEV(StorePtr));
  APInt Stride = getStoreStride(StoreEv);
  unsigned StoreSize = DL->getTypeStoreSize(SI->getValueOperand()->getType());
  bool NegStride = StoreSize == -Stride;

  // The store must be feeding a non-volatile load.
  LoadInst *LI = cast<LoadInst>(SI->getValueOperand());
  assert(LI->isUnordered() && "Expected only non-volatile non-ordered loads.");

  // See if the pointer expression is an AddRec like {base,+,1} on the current
  // loop, which indicates a strided load.  If we have something else, it's a
  // random load we can't handle.
  const SCEVAddRecExpr *LoadEv =
      cast<SCEVAddRecExpr>(SE->getSCEV(LI->getPointerOperand()));

  // The trip count of the loop and the base pointer of the addrec SCEV is
  // guaranteed to be loop invariant, which means that it should dominate the
  // header.  This allows us to insert code for it in the preheader.
  BasicBlock *Preheader = CurLoop->getLoopPreheader();
  IRBuilder<> Builder(Preheader->getTerminator());
  SCEVExpander Expander(*SE, *DL, "loop-idiom");

  const SCEV *StrStart = StoreEv->getStart();
  unsigned StrAS = SI->getPointerAddressSpace();
  Type *IntPtrTy = Builder.getIntPtrTy(*DL, StrAS);

  // Handle negative strided loops.
  if (NegStride)
    StrStart = getStartForNegStride(StrStart, BECount, IntPtrTy, StoreSize, SE);

  // Okay, we have a strided store "p[i]" of a loaded value.  We can turn
  // this into a memcpy in the loop preheader now if we want.  However, this
  // would be unsafe to do if there is anything else in the loop that may read
  // or write the memory region we're storing to.  This includes the load that
  // feeds the stores.  Check for an alias by generating the base address and
  // checking everything.
  Value *StoreBasePtr = Expander.expandCodeFor(
      StrStart, Builder.getInt8PtrTy(StrAS), Preheader->getTerminator());

  SmallPtrSet<Instruction *, 1> Stores;
  Stores.insert(SI);
  if (mayLoopAccessLocation(StoreBasePtr, ModRefInfo::ModRef, CurLoop, BECount,
                            StoreSize, *AA, Stores)) {
    Expander.clear();
    // If we generated new code for the base pointer, clean up.
    RecursivelyDeleteTriviallyDeadInstructions(StoreBasePtr, TLI);
    return false;
  }

  const SCEV *LdStart = LoadEv->getStart();
  unsigned LdAS = LI->getPointerAddressSpace();

  // Handle negative strided loops.
  if (NegStride)
    LdStart = getStartForNegStride(LdStart, BECount, IntPtrTy, StoreSize, SE);

  // For a memcpy, we have to make sure that the input array is not being
  // mutated by the loop.
  Value *LoadBasePtr = Expander.expandCodeFor(
      LdStart, Builder.getInt8PtrTy(LdAS), Preheader->getTerminator());

  if (mayLoopAccessLocation(LoadBasePtr, ModRefInfo::Mod, CurLoop, BECount,
                            StoreSize, *AA, Stores)) {
    Expander.clear();
    // If we generated new code for the base pointer, clean up.
    RecursivelyDeleteTriviallyDeadInstructions(LoadBasePtr, TLI);
    RecursivelyDeleteTriviallyDeadInstructions(StoreBasePtr, TLI);
    return false;
  }

  if (avoidLIRForMultiBlockLoop())
    return false;

  // Okay, everything is safe, we can transform this!

  const SCEV *NumBytesS =
      getNumBytes(BECount, IntPtrTy, StoreSize, CurLoop, DL, SE);

  Value *NumBytes =
      Expander.expandCodeFor(NumBytesS, IntPtrTy, Preheader->getTerminator());

  CallInst *NewCall = nullptr;
  // Check whether to generate an unordered atomic memcpy:
  //  If the load or store are atomic, then they must necessarily be unordered
  //  by previous checks.
  if (!SI->isAtomic() && !LI->isAtomic())
    NewCall = Builder.CreateMemCpy(StoreBasePtr, SI->getAlignment(),
                                   LoadBasePtr, LI->getAlignment(), NumBytes);
  else {
    // We cannot allow unaligned ops for unordered load/store, so reject
    // anything where the alignment isn't at least the element size.
    unsigned Align = std::min(SI->getAlignment(), LI->getAlignment());
    if (Align < StoreSize)
      return false;

    // If the element.atomic memcpy is not lowered into explicit
    // loads/stores later, then it will be lowered into an element-size
    // specific lib call. If the lib call doesn't exist for our store size, then
    // we shouldn't generate the memcpy.
    if (StoreSize > TTI->getAtomicMemIntrinsicMaxElementSize())
      return false;

    // Create the call.
    // Note that unordered atomic loads/stores are *required* by the spec to
    // have an alignment but non-atomic loads/stores may not.
    NewCall = Builder.CreateElementUnorderedAtomicMemCpy(
        StoreBasePtr, SI->getAlignment(), LoadBasePtr, LI->getAlignment(),
        NumBytes, StoreSize);
  }
  NewCall->setDebugLoc(SI->getDebugLoc());

  LLVM_DEBUG(dbgs() << "  Formed memcpy: " << *NewCall << "\n"
                    << "    from load ptr=" << *LoadEv << " at: " << *LI << "\n"
                    << "    from store ptr=" << *StoreEv << " at: " << *SI
                    << "\n");

  ORE.emit([&]() {
    return OptimizationRemark(DEBUG_TYPE, "ProcessLoopStoreOfLoopLoad",
                              NewCall->getDebugLoc(), Preheader)
           << "Formed a call to "
           << ore::NV("NewFunction", NewCall->getCalledFunction())
           << "() function";
  });

  // Okay, the memcpy has been formed.  Zap the original store and anything that
  // feeds into it.
  deleteDeadInstruction(SI);
  ++NumMemCpy;
  return true;
}

// When compiling for codesize we avoid idiom recognition for a multi-block loop
// unless it is a loop_memset idiom or a memset/memcpy idiom in a nested loop.
//
bool LoopIdiomRecognize::avoidLIRForMultiBlockLoop(bool IsMemset,
                                                   bool IsLoopMemset) {
  if (ApplyCodeSizeHeuristics && CurLoop->getNumBlocks() > 1) {
    if (!CurLoop->getParentLoop() && (!IsMemset || !IsLoopMemset)) {
      LLVM_DEBUG(dbgs() << "  " << CurLoop->getHeader()->getParent()->getName()
                        << " : LIR " << (IsMemset ? "Memset" : "Memcpy")
                        << " avoided: multi-block top-level loop\n");
      return true;
    }
  }

  return false;
}

bool LoopIdiomRecognize::runOnNoncountableLoop() {
  LLVM_DEBUG(dbgs() << DEBUG_TYPE " Scanning: F["
                    << CurLoop->getHeader()->getParent()->getName()
                    << "] Noncountable Loop %"
                    << CurLoop->getHeader()->getName() << "\n");

  return recognizeBCmp() || recognizePopcount() || recognizeAndInsertFFS();
}

/// Check if the given conditional branch is based on the comparison between
/// a variable and zero, and if the variable is non-zero or zero (JmpOnZero is
/// true), the control yields to the loop entry. If the branch matches the
/// behavior, the variable involved in the comparison is returned. This function
/// will be called to see if the precondition and postcondition of the loop are
/// in desirable form.
static Value *matchCondition(BranchInst *BI, BasicBlock *LoopEntry,
                             bool JmpOnZero = false) {
  if (!BI || !BI->isConditional())
    return nullptr;

  ICmpInst *Cond = dyn_cast<ICmpInst>(BI->getCondition());
  if (!Cond)
    return nullptr;

  ConstantInt *CmpZero = dyn_cast<ConstantInt>(Cond->getOperand(1));
  if (!CmpZero || !CmpZero->isZero())
    return nullptr;

  BasicBlock *TrueSucc = BI->getSuccessor(0);
  BasicBlock *FalseSucc = BI->getSuccessor(1);
  if (JmpOnZero)
    std::swap(TrueSucc, FalseSucc);

  ICmpInst::Predicate Pred = Cond->getPredicate();
  if ((Pred == ICmpInst::ICMP_NE && TrueSucc == LoopEntry) ||
      (Pred == ICmpInst::ICMP_EQ && FalseSucc == LoopEntry))
    return Cond->getOperand(0);

  return nullptr;
}

// Check if the recurrence variable `VarX` is in the right form to create
// the idiom. Returns the value coerced to a PHINode if so.
static PHINode *getRecurrenceVar(Value *VarX, Instruction *DefX,
                                 BasicBlock *LoopEntry) {
  auto *PhiX = dyn_cast<PHINode>(VarX);
  if (PhiX && PhiX->getParent() == LoopEntry &&
      (PhiX->getOperand(0) == DefX || PhiX->getOperand(1) == DefX))
    return PhiX;
  return nullptr;
}

/// Return true iff the idiom is detected in the loop.
///
/// Additionally:
/// 1) \p CntInst is set to the instruction counting the population bit.
/// 2) \p CntPhi is set to the corresponding phi node.
/// 3) \p Var is set to the value whose population bits are being counted.
///
/// The core idiom we are trying to detect is:
/// \code
///    if (x0 != 0)
///      goto loop-exit // the precondition of the loop
///    cnt0 = init-val;
///    do {
///       x1 = phi (x0, x2);
///       cnt1 = phi(cnt0, cnt2);
///
///       cnt2 = cnt1 + 1;
///        ...
///       x2 = x1 & (x1 - 1);
///        ...
///    } while(x != 0);
///
/// loop-exit:
/// \endcode
static bool detectPopcountIdiom(Loop *CurLoop, BasicBlock *PreCondBB,
                                Instruction *&CntInst, PHINode *&CntPhi,
                                Value *&Var) {
  // step 1: Check to see if the look-back branch match this pattern:
  //    "if (a!=0) goto loop-entry".
  BasicBlock *LoopEntry;
  Instruction *DefX2, *CountInst;
  Value *VarX1, *VarX0;
  PHINode *PhiX, *CountPhi;

  DefX2 = CountInst = nullptr;
  VarX1 = VarX0 = nullptr;
  PhiX = CountPhi = nullptr;
  LoopEntry = *(CurLoop->block_begin());

  // step 1: Check if the loop-back branch is in desirable form.
  {
    if (Value *T = matchCondition(
            dyn_cast<BranchInst>(LoopEntry->getTerminator()), LoopEntry))
      DefX2 = dyn_cast<Instruction>(T);
    else
      return false;
  }

  // step 2: detect instructions corresponding to "x2 = x1 & (x1 - 1)"
  {
    if (!DefX2 || DefX2->getOpcode() != Instruction::And)
      return false;

    BinaryOperator *SubOneOp;

    if ((SubOneOp = dyn_cast<BinaryOperator>(DefX2->getOperand(0))))
      VarX1 = DefX2->getOperand(1);
    else {
      VarX1 = DefX2->getOperand(0);
      SubOneOp = dyn_cast<BinaryOperator>(DefX2->getOperand(1));
    }
    if (!SubOneOp || SubOneOp->getOperand(0) != VarX1)
      return false;

    ConstantInt *Dec = dyn_cast<ConstantInt>(SubOneOp->getOperand(1));
    if (!Dec ||
        !((SubOneOp->getOpcode() == Instruction::Sub && Dec->isOne()) ||
          (SubOneOp->getOpcode() == Instruction::Add &&
           Dec->isMinusOne()))) {
      return false;
    }
  }

  // step 3: Check the recurrence of variable X
  PhiX = getRecurrenceVar(VarX1, DefX2, LoopEntry);
  if (!PhiX)
    return false;

  // step 4: Find the instruction which count the population: cnt2 = cnt1 + 1
  {
    CountInst = nullptr;
    for (BasicBlock::iterator Iter = LoopEntry->getFirstNonPHI()->getIterator(),
                              IterE = LoopEntry->end();
         Iter != IterE; Iter++) {
      Instruction *Inst = &*Iter;
      if (Inst->getOpcode() != Instruction::Add)
        continue;

      ConstantInt *Inc = dyn_cast<ConstantInt>(Inst->getOperand(1));
      if (!Inc || !Inc->isOne())
        continue;

      PHINode *Phi = getRecurrenceVar(Inst->getOperand(0), Inst, LoopEntry);
      if (!Phi)
        continue;

      // Check if the result of the instruction is live of the loop.
      bool LiveOutLoop = false;
      for (User *U : Inst->users()) {
        if ((cast<Instruction>(U))->getParent() != LoopEntry) {
          LiveOutLoop = true;
          break;
        }
      }

      if (LiveOutLoop) {
        CountInst = Inst;
        CountPhi = Phi;
        break;
      }
    }

    if (!CountInst)
      return false;
  }

  // step 5: check if the precondition is in this form:
  //   "if (x != 0) goto loop-head ; else goto somewhere-we-don't-care;"
  {
    auto *PreCondBr = dyn_cast<BranchInst>(PreCondBB->getTerminator());
    Value *T = matchCondition(PreCondBr, CurLoop->getLoopPreheader());
    if (T != PhiX->getOperand(0) && T != PhiX->getOperand(1))
      return false;

    CntInst = CountInst;
    CntPhi = CountPhi;
    Var = T;
  }

  return true;
}

/// Return true if the idiom is detected in the loop.
///
/// Additionally:
/// 1) \p CntInst is set to the instruction Counting Leading Zeros (CTLZ)
///       or nullptr if there is no such.
/// 2) \p CntPhi is set to the corresponding phi node
///       or nullptr if there is no such.
/// 3) \p Var is set to the value whose CTLZ could be used.
/// 4) \p DefX is set to the instruction calculating Loop exit condition.
///
/// The core idiom we are trying to detect is:
/// \code
///    if (x0 == 0)
///      goto loop-exit // the precondition of the loop
///    cnt0 = init-val;
///    do {
///       x = phi (x0, x.next);   //PhiX
///       cnt = phi(cnt0, cnt.next);
///
///       cnt.next = cnt + 1;
///        ...
///       x.next = x >> 1;   // DefX
///        ...
///    } while(x.next != 0);
///
/// loop-exit:
/// \endcode
static bool detectShiftUntilZeroIdiom(Loop *CurLoop, const DataLayout &DL,
                                      Intrinsic::ID &IntrinID, Value *&InitX,
                                      Instruction *&CntInst, PHINode *&CntPhi,
                                      Instruction *&DefX) {
  BasicBlock *LoopEntry;
  Value *VarX = nullptr;

  DefX = nullptr;
  CntInst = nullptr;
  CntPhi = nullptr;
  LoopEntry = *(CurLoop->block_begin());

  // step 1: Check if the loop-back branch is in desirable form.
  if (Value *T = matchCondition(
          dyn_cast<BranchInst>(LoopEntry->getTerminator()), LoopEntry))
    DefX = dyn_cast<Instruction>(T);
  else
    return false;

  // step 2: detect instructions corresponding to "x.next = x >> 1 or x << 1"
  if (!DefX || !DefX->isShift())
    return false;
  IntrinID = DefX->getOpcode() == Instruction::Shl ? Intrinsic::cttz :
                                                     Intrinsic::ctlz;
  ConstantInt *Shft = dyn_cast<ConstantInt>(DefX->getOperand(1));
  if (!Shft || !Shft->isOne())
    return false;
  VarX = DefX->getOperand(0);

  // step 3: Check the recurrence of variable X
  PHINode *PhiX = getRecurrenceVar(VarX, DefX, LoopEntry);
  if (!PhiX)
    return false;

  InitX = PhiX->getIncomingValueForBlock(CurLoop->getLoopPreheader());

  // Make sure the initial value can't be negative otherwise the ashr in the
  // loop might never reach zero which would make the loop infinite.
  if (DefX->getOpcode() == Instruction::AShr && !isKnownNonNegative(InitX, DL))
    return false;

  // step 4: Find the instruction which count the CTLZ: cnt.next = cnt + 1
  // TODO: We can skip the step. If loop trip count is known (CTLZ),
  //       then all uses of "cnt.next" could be optimized to the trip count
  //       plus "cnt0". Currently it is not optimized.
  //       This step could be used to detect POPCNT instruction:
  //       cnt.next = cnt + (x.next & 1)
  for (BasicBlock::iterator Iter = LoopEntry->getFirstNonPHI()->getIterator(),
                            IterE = LoopEntry->end();
       Iter != IterE; Iter++) {
    Instruction *Inst = &*Iter;
    if (Inst->getOpcode() != Instruction::Add)
      continue;

    ConstantInt *Inc = dyn_cast<ConstantInt>(Inst->getOperand(1));
    if (!Inc || !Inc->isOne())
      continue;

    PHINode *Phi = getRecurrenceVar(Inst->getOperand(0), Inst, LoopEntry);
    if (!Phi)
      continue;

    CntInst = Inst;
    CntPhi = Phi;
    break;
  }
  if (!CntInst)
    return false;

  return true;
}

/// Recognize CTLZ or CTTZ idiom in a non-countable loop and convert the loop
/// to countable (with CTLZ / CTTZ trip count). If CTLZ / CTTZ inserted as a new
/// trip count returns true; otherwise, returns false.
bool LoopIdiomRecognize::recognizeAndInsertFFS() {
  // Give up if the loop has multiple blocks or multiple backedges.
  if (CurLoop->getNumBackEdges() != 1 || CurLoop->getNumBlocks() != 1)
    return false;

  Intrinsic::ID IntrinID;
  Value *InitX;
  Instruction *DefX = nullptr;
  PHINode *CntPhi = nullptr;
  Instruction *CntInst = nullptr;
  // Help decide if transformation is profitable. For ShiftUntilZero idiom,
  // this is always 6.
  size_t IdiomCanonicalSize = 6;

  if (!detectShiftUntilZeroIdiom(CurLoop, *DL, IntrinID, InitX,
                                 CntInst, CntPhi, DefX))
    return false;

  bool IsCntPhiUsedOutsideLoop = false;
  for (User *U : CntPhi->users())
    if (!CurLoop->contains(cast<Instruction>(U))) {
      IsCntPhiUsedOutsideLoop = true;
      break;
    }
  bool IsCntInstUsedOutsideLoop = false;
  for (User *U : CntInst->users())
    if (!CurLoop->contains(cast<Instruction>(U))) {
      IsCntInstUsedOutsideLoop = true;
      break;
    }
  // If both CntInst and CntPhi are used outside the loop the profitability
  // is questionable.
  if (IsCntInstUsedOutsideLoop && IsCntPhiUsedOutsideLoop)
    return false;

  // For some CPUs result of CTLZ(X) intrinsic is undefined
  // when X is 0. If we can not guarantee X != 0, we need to check this
  // when expand.
  bool ZeroCheck = false;
  // It is safe to assume Preheader exist as it was checked in
  // parent function RunOnLoop.
  BasicBlock *PH = CurLoop->getLoopPreheader();

  // If we are using the count instruction outside the loop, make sure we
  // have a zero check as a precondition. Without the check the loop would run
  // one iteration for before any check of the input value. This means 0 and 1
  // would have identical behavior in the original loop and thus
  if (!IsCntPhiUsedOutsideLoop) {
    auto *PreCondBB = PH->getSinglePredecessor();
    if (!PreCondBB)
      return false;
    auto *PreCondBI = dyn_cast<BranchInst>(PreCondBB->getTerminator());
    if (!PreCondBI)
      return false;
    if (matchCondition(PreCondBI, PH) != InitX)
      return false;
    ZeroCheck = true;
  }

  // Check if CTLZ / CTTZ intrinsic is profitable. Assume it is always
  // profitable if we delete the loop.

  // the loop has only 6 instructions:
  //  %n.addr.0 = phi [ %n, %entry ], [ %shr, %while.cond ]
  //  %i.0 = phi [ %i0, %entry ], [ %inc, %while.cond ]
  //  %shr = ashr %n.addr.0, 1
  //  %tobool = icmp eq %shr, 0
  //  %inc = add nsw %i.0, 1
  //  br i1 %tobool

  const Value *Args[] =
      {InitX, ZeroCheck ? ConstantInt::getTrue(InitX->getContext())
                        : ConstantInt::getFalse(InitX->getContext())};

  // @llvm.dbg doesn't count as they have no semantic effect.
  auto InstWithoutDebugIt = CurLoop->getHeader()->instructionsWithoutDebug();
  uint32_t HeaderSize =
      std::distance(InstWithoutDebugIt.begin(), InstWithoutDebugIt.end());

  if (HeaderSize != IdiomCanonicalSize &&
      TTI->getIntrinsicCost(IntrinID, InitX->getType(), Args) >
          TargetTransformInfo::TCC_Basic)
    return false;

  transformLoopToCountable(IntrinID, PH, CntInst, CntPhi, InitX, DefX,
                           DefX->getDebugLoc(), ZeroCheck,
                           IsCntPhiUsedOutsideLoop);
  return true;
}

/// Recognizes a population count idiom in a non-countable loop.
///
/// If detected, transforms the relevant code to issue the popcount intrinsic
/// function call, and returns true; otherwise, returns false.
bool LoopIdiomRecognize::recognizePopcount() {
  if (TTI->getPopcntSupport(32) != TargetTransformInfo::PSK_FastHardware)
    return false;

  // Counting population are usually conducted by few arithmetic instructions.
  // Such instructions can be easily "absorbed" by vacant slots in a
  // non-compact loop. Therefore, recognizing popcount idiom only makes sense
  // in a compact loop.

  // Give up if the loop has multiple blocks or multiple backedges.
  if (CurLoop->getNumBackEdges() != 1 || CurLoop->getNumBlocks() != 1)
    return false;

  BasicBlock *LoopBody = *(CurLoop->block_begin());
  if (LoopBody->size() >= 20) {
    // The loop is too big, bail out.
    return false;
  }

  // It should have a preheader containing nothing but an unconditional branch.
  BasicBlock *PH = CurLoop->getLoopPreheader();
  if (!PH || &PH->front() != PH->getTerminator())
    return false;
  auto *EntryBI = dyn_cast<BranchInst>(PH->getTerminator());
  if (!EntryBI || EntryBI->isConditional())
    return false;

  // It should have a precondition block where the generated popcount intrinsic
  // function can be inserted.
  auto *PreCondBB = PH->getSinglePredecessor();
  if (!PreCondBB)
    return false;
  auto *PreCondBI = dyn_cast<BranchInst>(PreCondBB->getTerminator());
  if (!PreCondBI || PreCondBI->isUnconditional())
    return false;

  Instruction *CntInst;
  PHINode *CntPhi;
  Value *Val;
  if (!detectPopcountIdiom(CurLoop, PreCondBB, CntInst, CntPhi, Val))
    return false;

  transformLoopToPopcount(PreCondBB, CntInst, CntPhi, Val);
  return true;
}

static CallInst *createPopcntIntrinsic(IRBuilder<> &IRBuilder, Value *Val,
                                       const DebugLoc &DL) {
  Value *Ops[] = {Val};
  Type *Tys[] = {Val->getType()};

  Module *M = IRBuilder.GetInsertBlock()->getParent()->getParent();
  Function *Func = Intrinsic::getDeclaration(M, Intrinsic::ctpop, Tys);
  CallInst *CI = IRBuilder.CreateCall(Func, Ops);
  CI->setDebugLoc(DL);

  return CI;
}

static CallInst *createFFSIntrinsic(IRBuilder<> &IRBuilder, Value *Val,
                                    const DebugLoc &DL, bool ZeroCheck,
                                    Intrinsic::ID IID) {
  Value *Ops[] = {Val, ZeroCheck ? IRBuilder.getTrue() : IRBuilder.getFalse()};
  Type *Tys[] = {Val->getType()};

  Module *M = IRBuilder.GetInsertBlock()->getParent()->getParent();
  Function *Func = Intrinsic::getDeclaration(M, IID, Tys);
  CallInst *CI = IRBuilder.CreateCall(Func, Ops);
  CI->setDebugLoc(DL);

  return CI;
}

/// Transform the following loop (Using CTLZ, CTTZ is similar):
/// loop:
///   CntPhi = PHI [Cnt0, CntInst]
///   PhiX = PHI [InitX, DefX]
///   CntInst = CntPhi + 1
///   DefX = PhiX >> 1
///   LOOP_BODY
///   Br: loop if (DefX != 0)
/// Use(CntPhi) or Use(CntInst)
///
/// Into:
/// If CntPhi used outside the loop:
///   CountPrev = BitWidth(InitX) - CTLZ(InitX >> 1)
///   Count = CountPrev + 1
/// else
///   Count = BitWidth(InitX) - CTLZ(InitX)
/// loop:
///   CntPhi = PHI [Cnt0, CntInst]
///   PhiX = PHI [InitX, DefX]
///   PhiCount = PHI [Count, Dec]
///   CntInst = CntPhi + 1
///   DefX = PhiX >> 1
///   Dec = PhiCount - 1
///   LOOP_BODY
///   Br: loop if (Dec != 0)
/// Use(CountPrev + Cnt0) // Use(CntPhi)
/// or
/// Use(Count + Cnt0) // Use(CntInst)
///
/// If LOOP_BODY is empty the loop will be deleted.
/// If CntInst and DefX are not used in LOOP_BODY they will be removed.
void LoopIdiomRecognize::transformLoopToCountable(
    Intrinsic::ID IntrinID, BasicBlock *Preheader, Instruction *CntInst,
    PHINode *CntPhi, Value *InitX, Instruction *DefX, const DebugLoc &DL,
    bool ZeroCheck, bool IsCntPhiUsedOutsideLoop) {
  BranchInst *PreheaderBr = cast<BranchInst>(Preheader->getTerminator());

  // Step 1: Insert the CTLZ/CTTZ instruction at the end of the preheader block
  IRBuilder<> Builder(PreheaderBr);
  Builder.SetCurrentDebugLocation(DL);
  Value *FFS, *Count, *CountPrev, *NewCount, *InitXNext;

  //   Count = BitWidth - CTLZ(InitX);
  // If there are uses of CntPhi create:
  //   CountPrev = BitWidth - CTLZ(InitX >> 1);
  if (IsCntPhiUsedOutsideLoop) {
    if (DefX->getOpcode() == Instruction::AShr)
      InitXNext =
          Builder.CreateAShr(InitX, ConstantInt::get(InitX->getType(), 1));
    else if (DefX->getOpcode() == Instruction::LShr)
      InitXNext =
          Builder.CreateLShr(InitX, ConstantInt::get(InitX->getType(), 1));
    else if (DefX->getOpcode() == Instruction::Shl) // cttz
      InitXNext =
          Builder.CreateShl(InitX, ConstantInt::get(InitX->getType(), 1));
    else
      llvm_unreachable("Unexpected opcode!");
  } else
    InitXNext = InitX;
  FFS = createFFSIntrinsic(Builder, InitXNext, DL, ZeroCheck, IntrinID);
  Count = Builder.CreateSub(
      ConstantInt::get(FFS->getType(),
                       FFS->getType()->getIntegerBitWidth()),
      FFS);
  if (IsCntPhiUsedOutsideLoop) {
    CountPrev = Count;
    Count = Builder.CreateAdd(
        CountPrev,
        ConstantInt::get(CountPrev->getType(), 1));
  }

  NewCount = Builder.CreateZExtOrTrunc(
                      IsCntPhiUsedOutsideLoop ? CountPrev : Count,
                      cast<IntegerType>(CntInst->getType()));

  // If the counter's initial value is not zero, insert Add Inst.
  Value *CntInitVal = CntPhi->getIncomingValueForBlock(Preheader);
  ConstantInt *InitConst = dyn_cast<ConstantInt>(CntInitVal);
  if (!InitConst || !InitConst->isZero())
    NewCount = Builder.CreateAdd(NewCount, CntInitVal);

  // Step 2: Insert new IV and loop condition:
  // loop:
  //   ...
  //   PhiCount = PHI [Count, Dec]
  //   ...
  //   Dec = PhiCount - 1
  //   ...
  //   Br: loop if (Dec != 0)
  BasicBlock *Body = *(CurLoop->block_begin());
  auto *LbBr = cast<BranchInst>(Body->getTerminator());
  ICmpInst *LbCond = cast<ICmpInst>(LbBr->getCondition());
  Type *Ty = Count->getType();

  PHINode *TcPhi = PHINode::Create(Ty, 2, "tcphi", &Body->front());

  Builder.SetInsertPoint(LbCond);
  Instruction *TcDec = cast<Instruction>(
      Builder.CreateSub(TcPhi, ConstantInt::get(Ty, 1),
                        "tcdec", false, true));

  TcPhi->addIncoming(Count, Preheader);
  TcPhi->addIncoming(TcDec, Body);

  CmpInst::Predicate Pred =
      (LbBr->getSuccessor(0) == Body) ? CmpInst::ICMP_NE : CmpInst::ICMP_EQ;
  LbCond->setPredicate(Pred);
  LbCond->setOperand(0, TcDec);
  LbCond->setOperand(1, ConstantInt::get(Ty, 0));

  // Step 3: All the references to the original counter outside
  //  the loop are replaced with the NewCount
  if (IsCntPhiUsedOutsideLoop)
    CntPhi->replaceUsesOutsideBlock(NewCount, Body);
  else
    CntInst->replaceUsesOutsideBlock(NewCount, Body);

  // step 4: Forget the "non-computable" trip-count SCEV associated with the
  //   loop. The loop would otherwise not be deleted even if it becomes empty.
  SE->forgetLoop(CurLoop);
}

void LoopIdiomRecognize::transformLoopToPopcount(BasicBlock *PreCondBB,
                                                 Instruction *CntInst,
                                                 PHINode *CntPhi, Value *Var) {
  BasicBlock *PreHead = CurLoop->getLoopPreheader();
  auto *PreCondBr = cast<BranchInst>(PreCondBB->getTerminator());
  const DebugLoc &DL = CntInst->getDebugLoc();

  // Assuming before transformation, the loop is following:
  //  if (x) // the precondition
  //     do { cnt++; x &= x - 1; } while(x);

  // Step 1: Insert the ctpop instruction at the end of the precondition block
  IRBuilder<> Builder(PreCondBr);
  Value *PopCnt, *PopCntZext, *NewCount, *TripCnt;
  {
    PopCnt = createPopcntIntrinsic(Builder, Var, DL);
    NewCount = PopCntZext =
        Builder.CreateZExtOrTrunc(PopCnt, cast<IntegerType>(CntPhi->getType()));

    if (NewCount != PopCnt)
      (cast<Instruction>(NewCount))->setDebugLoc(DL);

    // TripCnt is exactly the number of iterations the loop has
    TripCnt = NewCount;

    // If the population counter's initial value is not zero, insert Add Inst.
    Value *CntInitVal = CntPhi->getIncomingValueForBlock(PreHead);
    ConstantInt *InitConst = dyn_cast<ConstantInt>(CntInitVal);
    if (!InitConst || !InitConst->isZero()) {
      NewCount = Builder.CreateAdd(NewCount, CntInitVal);
      (cast<Instruction>(NewCount))->setDebugLoc(DL);
    }
  }

  // Step 2: Replace the precondition from "if (x == 0) goto loop-exit" to
  //   "if (NewCount == 0) loop-exit". Without this change, the intrinsic
  //   function would be partial dead code, and downstream passes will drag
  //   it back from the precondition block to the preheader.
  {
    ICmpInst *PreCond = cast<ICmpInst>(PreCondBr->getCondition());

    Value *Opnd0 = PopCntZext;
    Value *Opnd1 = ConstantInt::get(PopCntZext->getType(), 0);
    if (PreCond->getOperand(0) != Var)
      std::swap(Opnd0, Opnd1);

    ICmpInst *NewPreCond = cast<ICmpInst>(
        Builder.CreateICmp(PreCond->getPredicate(), Opnd0, Opnd1));
    PreCondBr->setCondition(NewPreCond);

    RecursivelyDeleteTriviallyDeadInstructions(PreCond, TLI);
  }

  // Step 3: Note that the population count is exactly the trip count of the
  // loop in question, which enable us to convert the loop from noncountable
  // loop into a countable one. The benefit is twofold:
  //
  //  - If the loop only counts population, the entire loop becomes dead after
  //    the transformation. It is a lot easier to prove a countable loop dead
  //    than to prove a noncountable one. (In some C dialects, an infinite loop
  //    isn't dead even if it computes nothing useful. In general, DCE needs
  //    to prove a noncountable loop finite before safely delete it.)
  //
  //  - If the loop also performs something else, it remains alive.
  //    Since it is transformed to countable form, it can be aggressively
  //    optimized by some optimizations which are in general not applicable
  //    to a noncountable loop.
  //
  // After this step, this loop (conceptually) would look like following:
  //   newcnt = __builtin_ctpop(x);
  //   t = newcnt;
  //   if (x)
  //     do { cnt++; x &= x-1; t--) } while (t > 0);
  BasicBlock *Body = *(CurLoop->block_begin());
  {
    auto *LbBr = cast<BranchInst>(Body->getTerminator());
    ICmpInst *LbCond = cast<ICmpInst>(LbBr->getCondition());
    Type *Ty = TripCnt->getType();

    PHINode *TcPhi = PHINode::Create(Ty, 2, "tcphi", &Body->front());

    Builder.SetInsertPoint(LbCond);
    Instruction *TcDec = cast<Instruction>(
        Builder.CreateSub(TcPhi, ConstantInt::get(Ty, 1),
                          "tcdec", false, true));

    TcPhi->addIncoming(TripCnt, PreHead);
    TcPhi->addIncoming(TcDec, Body);

    CmpInst::Predicate Pred =
        (LbBr->getSuccessor(0) == Body) ? CmpInst::ICMP_UGT : CmpInst::ICMP_SLE;
    LbCond->setPredicate(Pred);
    LbCond->setOperand(0, TcDec);
    LbCond->setOperand(1, ConstantInt::get(Ty, 0));
  }

  // Step 4: All the references to the original population counter outside
  //  the loop are replaced with the NewCount -- the value returned from
  //  __builtin_ctpop().
  CntInst->replaceUsesOutsideBlock(NewCount, Body);

  // step 5: Forget the "non-computable" trip-count SCEV associated with the
  //   loop. The loop would otherwise not be deleted even if it becomes empty.
  SE->forgetLoop(CurLoop);
}

bool LoopIdiomRecognize::matchBCmpLoopStructure(
    CmpLoopStructure &CmpLoop) const {
  ICmpInst::Predicate BCmpPred;

  // We are looking for the following basic layout:
  //  PreheaderBB: <preheader>              ; preds = ???
  //    <...>
  //    br label %LoopHeaderBB
  //  LoopHeaderBB: <header,exiting>        ; preds = %PreheaderBB,%LoopLatchBB
  //    <...>
  //    %BCmpValue = icmp <...>
  //    br i1 %BCmpValue, label %LoopLatchBB, label %Successor0
  //  LoopLatchBB: <latch,exiting>          ; preds = %LoopHeaderBB
  //    <...>
  //    %LatchCmpValue = <are we done, or do next iteration?>
  //    br i1 %LatchCmpValue, label %Successor1, label %LoopHeaderBB
  //  Successor0: <exit>                    ; preds = %LoopHeaderBB
  //    <...>
  //  Successor1: <exit>                    ; preds = %LoopLatchBB
  //    <...>
  //
  // Successor0 and Successor1 may or may not be the same basic block.

  // Match basic frame-work of this supposedly-comparison loop.
  using namespace PatternMatch;
  if (!match(CurLoop->getHeader()->getTerminator(),
             m_Br(m_CombineAnd(m_ICmp(BCmpPred, m_Value(), m_Value()),
                               m_Value(CmpLoop.BCmpValue)),
                  CmpLoop.HeaderBrEqualBB, CmpLoop.HeaderBrUnequalBB)) ||
      !match(CurLoop->getLoopLatch()->getTerminator(),
             m_Br(m_CombineAnd(m_Cmp(), m_Value(CmpLoop.LatchCmpValue)),
                  CmpLoop.LatchBrFinishBB, CmpLoop.LatchBrContinueBB))) {
    LLVM_DEBUG(dbgs() << "Basic control-flow layout unrecognized.\n");
    return false;
  }
  LLVM_DEBUG(dbgs() << "Recognized basic control-flow layout.\n");
  return true;
}

bool LoopIdiomRecognize::matchBCmpOfLoads(Value *BCmpValue,
                                          CmpOfLoads &CmpOfLoads) const {
  using namespace PatternMatch;
  LLVM_DEBUG(dbgs() << "Analyzing header icmp " << *BCmpValue
                    << "   as bcmp pattern.\n");

  // Match bcmp-style loop header cmp. It must be an eq-icmp of loads. Example:
  //    %v0 = load <...>, <...>* %LoadSrcA
  //    %v1 = load <...>, <...>* %LoadSrcB
  //    %CmpLoop.BCmpValue = icmp eq <...> %v0, %v1
  // There won't be any no-op bitcasts between load and icmp,
  // they would have been transformed into a load of bitcast.
  // FIXME: {b,mem}cmp() calls have the same semantics as icmp. Match them too.
  if (!match(BCmpValue,
             m_ICmp(CmpOfLoads.BCmpPred,
                    m_CombineAnd(m_Load(m_Value(CmpOfLoads.LoadSrcA)),
                                 m_Value(CmpOfLoads.LoadA)),
                    m_CombineAnd(m_Load(m_Value(CmpOfLoads.LoadSrcB)),
                                 m_Value(CmpOfLoads.LoadB)))) ||
      !ICmpInst::isEquality(CmpOfLoads.BCmpPred)) {
    LLVM_DEBUG(dbgs() << "Loop header icmp did not match bcmp pattern.\n");
    return false;
  }
  LLVM_DEBUG(dbgs() << "Recognized header icmp as bcmp pattern with loads:\n\t"
                    << *CmpOfLoads.LoadA << "\n\t" << *CmpOfLoads.LoadB
                    << "\n");
  // FIXME: handle memcmp pattern?
  return true;
}

bool LoopIdiomRecognize::recognizeBCmpLoopControlFlow(
    const CmpOfLoads &CmpOfLoads, CmpLoopStructure &CmpLoop) const {
  BasicBlock *LoopHeaderBB = CurLoop->getHeader();
  BasicBlock *LoopLatchBB = CurLoop->getLoopLatch();

  // Be wary, comparisons can be inverted, canonicalize order.
  // If this 'element' comparison passed, we expect to proceed to the next elt.
  if (CmpOfLoads.BCmpPred != ICmpInst::Predicate::ICMP_EQ)
    std::swap(CmpLoop.HeaderBrEqualBB, CmpLoop.HeaderBrUnequalBB);
  // The predicate on loop latch does not matter, just canonicalize some order.
  if (CmpLoop.LatchBrContinueBB != LoopHeaderBB)
    std::swap(CmpLoop.LatchBrFinishBB, CmpLoop.LatchBrContinueBB);

  // Check that control-flow between blocks is as expected.
  if (CmpLoop.HeaderBrEqualBB != LoopLatchBB ||
      CmpLoop.LatchBrContinueBB != LoopHeaderBB) {
    LLVM_DEBUG(dbgs() << "Loop control-flow not recognized.\n");
    return false;
  }

  SmallVector<BasicBlock *, 2> ExitBlocks;
  CurLoop->getUniqueExitBlocks(ExitBlocks);
  assert(ExitBlocks.size() <= 2U && "Can't have more than two exit blocks.");

  assert(!is_contained(ExitBlocks, CmpLoop.HeaderBrEqualBB) &&
         is_contained(ExitBlocks, CmpLoop.HeaderBrUnequalBB) &&
         !is_contained(ExitBlocks, CmpLoop.LatchBrContinueBB) &&
         is_contained(ExitBlocks, CmpLoop.LatchBrFinishBB) &&
         "Unexpected exit edges.");

  LLVM_DEBUG(dbgs() << "Recognized loop control-flow.\n");

  LLVM_DEBUG(dbgs() << "Performing side-effect analysis on the loop.\n");
  assert(CurLoop->isLCSSAForm(*DT) && "Should only get LCSSA-form loops here.");
  // No loop instructions must be used outside of the loop. Since we are in
  // LCSSA form, we only need to check successor block's PHI nodes's incoming
  // values for incoming blocks that are the loop basic blocks.
  for (const BasicBlock *ExitBB : ExitBlocks) {
    for (const PHINode &PHI : ExitBB->phis()) {
      for (const BasicBlock *LoopBB :
           make_filter_range(PHI.blocks(), [this](BasicBlock *PredecessorBB) {
             return CurLoop->contains(PredecessorBB);
           })) {
        const auto *I =
            dyn_cast<Instruction>(PHI.getIncomingValueForBlock(LoopBB));
        if (I && CurLoop->contains(I)) {
          LLVM_DEBUG(dbgs()
                     << "Loop contains instruction " << *I
                     << "   which is used outside of the loop in basic block  "
                     << ExitBB->getName() << "  in phi node  " << PHI << "\n");
          return false;
        }
      }
    }
  }
  // Similarly, the loop should not have any other observable side-effects
  // other than the final comparison result.
  for (BasicBlock *LoopBB : CurLoop->blocks()) {
    for (Instruction &I : *LoopBB) {
      if (isa<DbgInfoIntrinsic>(I)) // Ignore dbginfo.
        continue;                   // FIXME: anything else? lifetime info?
      if ((I.mayHaveSideEffects() || I.isAtomic() || I.isFenceLike()) &&
          &I != CmpOfLoads.LoadA && &I != CmpOfLoads.LoadB) {
        LLVM_DEBUG(
            dbgs() << "Loop contains instruction with potential side-effects: "
                   << I << "\n");
        return false;
      }
    }
  }
  LLVM_DEBUG(dbgs() << "No loop instructions deemed to have side-effects.\n");
  return true;
}

bool LoopIdiomRecognize::recognizeBCmpLoopSCEV(uint64_t BCmpTyBytes,
                                               CmpOfLoads &CmpOfLoads,
                                               const SCEV *&SrcA,
                                               const SCEV *&SrcB,
                                               const SCEV *&Iterations) const {
  // Try to compute SCEV of the loads, for this loop's scope.
  const auto *ScevForSrcA = dyn_cast<SCEVAddRecExpr>(
      SE->getSCEVAtScope(CmpOfLoads.LoadSrcA, CurLoop));
  const auto *ScevForSrcB = dyn_cast<SCEVAddRecExpr>(
      SE->getSCEVAtScope(CmpOfLoads.LoadSrcB, CurLoop));
  if (!ScevForSrcA || !ScevForSrcB) {
    LLVM_DEBUG(dbgs() << "Failed to get SCEV expressions for load sources.\n");
    return false;
  }

  LLVM_DEBUG(dbgs() << "Got SCEV expressions (at loop scope) for loads:\n\t"
                    << *ScevForSrcA << "\n\t" << *ScevForSrcB << "\n");

  // Loads must have folloving SCEV exprs:  {%ptr,+,BCmpTyBytes}<%LoopHeaderBB>
  const SCEV *RecStepForA = ScevForSrcA->getStepRecurrence(*SE);
  const SCEV *RecStepForB = ScevForSrcB->getStepRecurrence(*SE);
  if (!ScevForSrcA->isAffine() || !ScevForSrcB->isAffine() ||
      ScevForSrcA->getLoop() != CurLoop || ScevForSrcB->getLoop() != CurLoop ||
      RecStepForA != RecStepForB || !isa<SCEVConstant>(RecStepForA) ||
      cast<SCEVConstant>(RecStepForA)->getAPInt() != BCmpTyBytes) {
    LLVM_DEBUG(dbgs() << "Unsupported SCEV expressions for loads. Only support "
                         "affine SCEV expressions originating in the loop we "
                         "are analysing with identical constant positive step, "
                         "equal to the count of bytes compared. Got:\n\t"
                      << *RecStepForA << "\n\t" << *RecStepForB << "\n");
    return false;
    // FIXME: can support BCmpTyBytes > Step.
    // But will need to account for the extra bytes compared at the end.
  }

  SrcA = ScevForSrcA->getStart();
  SrcB = ScevForSrcB->getStart();
  LLVM_DEBUG(dbgs() << "Got SCEV expressions for load sources:\n\t" << *SrcA
                    << "\n\t" << *SrcB << "\n");

  // The load sources must be loop-invants that dominate the loop header.
  if (SrcA == SE->getCouldNotCompute() || SrcB == SE->getCouldNotCompute() ||
      !SE->isAvailableAtLoopEntry(SrcA, CurLoop) ||
      !SE->isAvailableAtLoopEntry(SrcB, CurLoop)) {
    LLVM_DEBUG(dbgs() << "Unsupported SCEV expressions for loads, unavaliable "
                         "prior to loop header.\n");
    return false;
  }

  LLVM_DEBUG(dbgs() << "SCEV expressions for loads are acceptable.\n");

  // bcmp / memcmp take length argument as size_t, so let's conservatively
  // assume that the iteration count should be not wider than that.
  Type *CmpFuncSizeTy = DL->getIntPtrType(SE->getContext());

  // For how many iterations is loop guaranteed not to exit via LoopLatch?
  // This is one less than the maximal number of comparisons,and is:  n + -1
  const SCEV *LoopExitCount =
      SE->getExitCount(CurLoop, CurLoop->getLoopLatch());
  LLVM_DEBUG(dbgs() << "Got SCEV expression for loop latch exit count: "
                    << *LoopExitCount << "\n");
  // Exit count, similarly, must be loop-invant that dominates the loop header.
  if (LoopExitCount == SE->getCouldNotCompute() ||
      !LoopExitCount->getType()->isIntOrPtrTy() ||
      LoopExitCount->getType()->getScalarSizeInBits() >
          CmpFuncSizeTy->getScalarSizeInBits() ||
      !SE->isAvailableAtLoopEntry(LoopExitCount, CurLoop)) {
    LLVM_DEBUG(dbgs() << "Unsupported SCEV expression for loop latch exit.\n");
    return false;
  }

  // LoopExitCount is always one less than the actual count of iterations.
  // Do this before cast, else we will be stuck with   1 + zext(-1 + n)
  Iterations = SE->getAddExpr(
      LoopExitCount, SE->getOne(LoopExitCount->getType()), SCEV::FlagNUW);
  assert(Iterations != SE->getCouldNotCompute() &&
         "Shouldn't fail to increment by one.");

  LLVM_DEBUG(dbgs() << "Computed iteration count: " << *Iterations << "\n");
  return true;
}

/// Return true iff the bcmp idiom is detected in the loop.
///
/// Additionally:
/// 1) \p BCmpInst is set to the root byte-comparison instruction.
/// 2) \p LatchCmpInst is set to the comparison that controls the latch.
/// 3) \p LoadA is set to the first  LoadInst.
/// 4) \p LoadB is set to the second LoadInst.
/// 5) \p SrcA is set to the first  source location that is being compared.
/// 6) \p SrcB is set to the second source location that is being compared.
/// 7) \p NBytes is set to the number of bytes to compare.
bool LoopIdiomRecognize::detectBCmpIdiom(ICmpInst *&BCmpInst,
                                         CmpInst *&LatchCmpInst,
                                         LoadInst *&LoadA, LoadInst *&LoadB,
                                         const SCEV *&SrcA, const SCEV *&SrcB,
                                         const SCEV *&NBytes) const {
  LLVM_DEBUG(dbgs() << "Recognizing bcmp idiom\n");

  // Give up if the loop is not in normal form, or has more than 2 blocks.
  if (!CurLoop->isLoopSimplifyForm() || CurLoop->getNumBlocks() > 2) {
    LLVM_DEBUG(dbgs() << "Basic loop structure unrecognized.\n");
    return false;
  }
  LLVM_DEBUG(dbgs() << "Recognized basic loop structure.\n");

  CmpLoopStructure CmpLoop;
  if (!matchBCmpLoopStructure(CmpLoop))
    return false;

  CmpOfLoads CmpOfLoads;
  if (!matchBCmpOfLoads(CmpLoop.BCmpValue, CmpOfLoads))
    return false;

  if (!recognizeBCmpLoopControlFlow(CmpOfLoads, CmpLoop))
    return false;

  BCmpInst = cast<ICmpInst>(CmpLoop.BCmpValue);        // FIXME: is there no
  LatchCmpInst = cast<CmpInst>(CmpLoop.LatchCmpValue); // way to combine
  LoadA = cast<LoadInst>(CmpOfLoads.LoadA);            // these cast with
  LoadB = cast<LoadInst>(CmpOfLoads.LoadB);            // m_Value() matcher?

  Type *BCmpValTy = BCmpInst->getOperand(0)->getType();
  LLVMContext &Context = BCmpValTy->getContext();
  uint64_t BCmpTyBits = DL->getTypeSizeInBits(BCmpValTy);
  static constexpr uint64_t ByteTyBits = 8;

  LLVM_DEBUG(dbgs() << "Got comparison between values of type " << *BCmpValTy
                    << " of size " << BCmpTyBits
                    << " bits (while byte = " << ByteTyBits << " bits).\n");
  // bcmp()/memcmp() minimal unit of work is a byte. Therefore we must check
  // that we are dealing with a multiple of a byte here.
  if (BCmpTyBits % ByteTyBits != 0) {
    LLVM_DEBUG(dbgs() << "Value size is not a multiple of byte.\n");
    return false;
    // FIXME: could still be done under a run-time check that the total bit
    // count is a multiple of a byte i guess? Or handle remainder separately?
  }

  // Each comparison is done on this many bytes.
  uint64_t BCmpTyBytes = BCmpTyBits / ByteTyBits;
  LLVM_DEBUG(dbgs() << "Size is exactly " << BCmpTyBytes
                    << " bytes, eligible for bcmp conversion.\n");

  const SCEV *Iterations;
  if (!recognizeBCmpLoopSCEV(BCmpTyBytes, CmpOfLoads, SrcA, SrcB, Iterations))
    return false;

  // bcmp / memcmp take length argument as size_t, do promotion now.
  Type *CmpFuncSizeTy = DL->getIntPtrType(Context);
  Iterations = SE->getNoopOrZeroExtend(Iterations, CmpFuncSizeTy);
  assert(Iterations != SE->getCouldNotCompute() && "Promotion failed.");
  // Note that it didn't do ptrtoint cast, we will need to do it manually.

  // We will be comparing *bytes*, not BCmpTy, we need to recalculate size.
  // It's a multiplication, and it *could* overflow. But for it to overflow
  // we'd want to compare more bytes than could be represented by size_t, But
  // allocation functions also take size_t. So how'd you produce such buffer?
  // FIXME: we likely need to actually check that we know this won't overflow,
  //        via llvm::computeOverflowForUnsignedMul().
  NBytes = SE->getMulExpr(
      Iterations, SE->getConstant(CmpFuncSizeTy, BCmpTyBytes), SCEV::FlagNUW);
  assert(NBytes != SE->getCouldNotCompute() &&
         "Shouldn't fail to increment by one.");

  LLVM_DEBUG(dbgs() << "Computed total byte count: " << *NBytes << "\n");

  if (LoadA->getPointerAddressSpace() != LoadB->getPointerAddressSpace() ||
      LoadA->getPointerAddressSpace() != 0 || !LoadA->isSimple() ||
      !LoadB->isSimple()) {
    StringLiteral L("Unsupported loads in idiom - only support identical, "
                    "simple loads from address space 0.\n");
    LLVM_DEBUG(dbgs() << L);
    ORE.emit([&]() {
      return OptimizationRemarkMissed(DEBUG_TYPE, "BCmpIdiomUnsupportedLoads",
                                      BCmpInst->getDebugLoc(),
                                      CurLoop->getHeader())
             << L;
    });
    return false; // FIXME: support non-simple loads.
  }

  LLVM_DEBUG(dbgs() << "Recognized bcmp idiom\n");
  ORE.emit([&]() {
    return OptimizationRemarkAnalysis(DEBUG_TYPE, "RecognizedBCmpIdiom",
                                      CurLoop->getStartLoc(),
                                      CurLoop->getHeader())
           << "Loop recognized as a bcmp idiom";
  });

  return true;
}

BasicBlock *
LoopIdiomRecognize::transformBCmpControlFlow(ICmpInst *ComparedEqual) {
  LLVM_DEBUG(dbgs() << "Transforming control-flow.\n");
  SmallVector<DominatorTree::UpdateType, 8> DTUpdates;

  BasicBlock *PreheaderBB = CurLoop->getLoopPreheader();
  BasicBlock *HeaderBB = CurLoop->getHeader();
  BasicBlock *LoopLatchBB = CurLoop->getLoopLatch();
  SmallString<32> LoopName = CurLoop->getName();
  Function *Func = PreheaderBB->getParent();
  LLVMContext &Context = Func->getContext();

  // Before doing anything, drop SCEV info.
  SE->forgetLoop(CurLoop);

  // Here we start with: (0/6)
  //  PreheaderBB: <preheader>        ; preds = ???
  //    <...>
  //    %memcmp = call i32 @memcmp(i8* %LoadSrcA, i8* %LoadSrcB, i64 %Nbytes)
  //    %ComparedEqual = icmp eq <...> %memcmp, 0
  //    br label %LoopHeaderBB
  //  LoopHeaderBB: <header,exiting>  ; preds = %PreheaderBB,%LoopLatchBB
  //    <...>
  //    br i1 %<...>, label %LoopLatchBB, label %Successor0BB
  //  LoopLatchBB: <latch,exiting>    ; preds = %LoopHeaderBB
  //    <...>
  //    br i1 %<...>, label %Successor1BB, label %LoopHeaderBB
  //  Successor0BB: <exit>            ; preds = %LoopHeaderBB
  //    %S0PHI = phi <...> [ <...>, %LoopHeaderBB ]
  //    <...>
  //  Successor1BB: <exit>            ; preds = %LoopLatchBB
  //    %S1PHI = phi <...> [ <...>, %LoopLatchBB ]
  //    <...>
  //
  // Successor0 and Successor1 may or may not be the same basic block.

  // Decouple the edge between loop preheader basic block and loop header basic
  // block. Thus the loop has become unreachable.
  assert(cast<BranchInst>(PreheaderBB->getTerminator())->isUnconditional() &&
         PreheaderBB->getTerminator()->getSuccessor(0) == HeaderBB &&
         "Preheader bb must end with an unconditional branch to header bb.");
  PreheaderBB->getTerminator()->eraseFromParent();
  DTUpdates.push_back({DominatorTree::Delete, PreheaderBB, HeaderBB});

  // Create a new preheader basic block before loop header basic block.
  auto *PhonyPreheaderBB = BasicBlock::Create(
      Context, LoopName + ".phonypreheaderbb", Func, HeaderBB);
  // And insert an unconditional branch from phony preheader basic block to
  // loop header basic block.
  IRBuilder<>(PhonyPreheaderBB).CreateBr(HeaderBB);
  DTUpdates.push_back({DominatorTree::Insert, PhonyPreheaderBB, HeaderBB});

  // Create a *single* new empty block that we will substitute as a
  // successor basic block for the loop's exits. This one is temporary.
  // Much like phony preheader basic block, it is not connected.
  auto *PhonySuccessorBB =
      BasicBlock::Create(Context, LoopName + ".phonysuccessorbb", Func,
                         LoopLatchBB->getNextNode());
  // That block must have *some* non-PHI instruction, or else deleteDeadLoop()
  // will mess up cleanup of dbginfo, and verifier will complain.
  IRBuilder<>(PhonySuccessorBB).CreateUnreachable();

  // Create two new empty blocks that we will use to preserve the original
  // loop exit control-flow, and preserve the incoming values in the PHI nodes
  // in loop's successor exit blocks. These will live one.
  auto *ComparedUnequalBB =
      BasicBlock::Create(Context, ComparedEqual->getName() + ".unequalbb", Func,
                         PhonySuccessorBB->getNextNode());
  auto *ComparedEqualBB =
      BasicBlock::Create(Context, ComparedEqual->getName() + ".equalbb", Func,
                         PhonySuccessorBB->getNextNode());

  // By now we have: (1/6)
  //  PreheaderBB:                    ; preds = ???
  //    <...>
  //    %memcmp = call i32 @memcmp(i8* %LoadSrcA, i8* %LoadSrcB, i64 %Nbytes)
  //    %ComparedEqual = icmp eq <...> %memcmp, 0
  //    [no terminator instruction!]
  //  PhonyPreheaderBB: <preheader>   ; No preds, UNREACHABLE!
  //    br label %LoopHeaderBB
  //  LoopHeaderBB: <header,exiting>  ; preds = %PhonyPreheaderBB, %LoopLatchBB
  //    <...>
  //    br i1 %<...>, label %LoopLatchBB, label %Successor0BB
  //  LoopLatchBB: <latch,exiting>    ; preds = %LoopHeaderBB
  //    <...>
  //    br i1 %<...>, label %Successor1BB, label %LoopHeaderBB
  //  PhonySuccessorBB:               ; No preds, UNREACHABLE!
  //    unreachable
  //  EqualBB:                        ; No preds, UNREACHABLE!
  //    [no terminator instruction!]
  //  UnequalBB:                      ; No preds, UNREACHABLE!
  //    [no terminator instruction!]
  //  Successor0BB: <exit>            ; preds = %LoopHeaderBB
  //    %S0PHI = phi <...> [ <...>, %LoopHeaderBB ]
  //    <...>
  //  Successor1BB: <exit>            ; preds = %LoopLatchBB
  //    %S1PHI = phi <...> [ <...>, %LoopLatchBB ]
  //    <...>

  // What is the mapping/replacement basic block for exiting out of the loop
  // from either of old's loop basic blocks?
  auto GetReplacementBB = [this, ComparedEqualBB,
                           ComparedUnequalBB](const BasicBlock *OldBB) {
    assert(CurLoop->contains(OldBB) && "Only for loop's basic blocks.");
    if (OldBB == CurLoop->getLoopLatch()) // "all elements compared equal".
      return ComparedEqualBB;
    if (OldBB == CurLoop->getHeader()) // "element compared unequal".
      return ComparedUnequalBB;
    llvm_unreachable("Only had two basic blocks in loop.");
  };

  // What are the exits out of this loop?
  SmallVector<Loop::Edge, 2> LoopExitEdges;
  CurLoop->getExitEdges(LoopExitEdges);
  assert(LoopExitEdges.size() == 2 && "Should have only to two exit edges.");

  // Populate new basic blocks, update the exiting control-flow, PHI nodes.
  for (const Loop::Edge &Edge : LoopExitEdges) {
    auto *OldLoopBB = const_cast<BasicBlock *>(Edge.first);
    auto *SuccessorBB = const_cast<BasicBlock *>(Edge.second);
    assert(CurLoop->contains(OldLoopBB) && !CurLoop->contains(SuccessorBB) &&
           "Unexpected edge.");

    // If we would exit the loop from this loop's basic block,
    // what semantically would that mean? Did comparison succeed or fail?
    BasicBlock *NewBB = GetReplacementBB(OldLoopBB);
    assert(NewBB->empty() && "Should not get same new basic block here twice.");
    IRBuilder<> Builder(NewBB);
    Builder.SetCurrentDebugLocation(OldLoopBB->getTerminator()->getDebugLoc());
    Builder.CreateBr(SuccessorBB);
    DTUpdates.push_back({DominatorTree::Insert, NewBB, SuccessorBB});
    // Also, be *REALLY* careful with PHI nodes in successor basic block,
    // update them to recieve the same input value, but not from current loop's
    // basic block, but from new basic block instead.
    SuccessorBB->replacePhiUsesWith(OldLoopBB, NewBB);
    // Also, change loop control-flow. This loop's basic block shall no longer
    // exit from the loop to it's original successor basic block, but to our new
    // phony successor basic block. Note that new successor will be unique exit.
    OldLoopBB->getTerminator()->replaceSuccessorWith(SuccessorBB,
                                                     PhonySuccessorBB);
    DTUpdates.push_back({DominatorTree::Delete, OldLoopBB, SuccessorBB});
    DTUpdates.push_back({DominatorTree::Insert, OldLoopBB, PhonySuccessorBB});
  }

  // Inform DomTree about edge changes. Note that LoopInfo is still out-of-date.
  assert(DTUpdates.size() == 8 && "Update count prediction failed.");
  DomTreeUpdater DTU(DT, DomTreeUpdater::UpdateStrategy::Eager);
  DTU.applyUpdates(DTUpdates);
  DTUpdates.clear();

  // By now we have: (2/6)
  //  PreheaderBB:                    ; preds = ???
  //    <...>
  //    %memcmp = call i32 @memcmp(i8* %LoadSrcA, i8* %LoadSrcB, i64 %Nbytes)
  //    %ComparedEqual = icmp eq <...> %memcmp, 0
  //    [no terminator instruction!]
  //  PhonyPreheaderBB: <preheader>   ; No preds, UNREACHABLE!
  //    br label %LoopHeaderBB
  //  LoopHeaderBB: <header,exiting>  ; preds = %PhonyPreheaderBB, %LoopLatchBB
  //    <...>
  //    br i1 %<...>, label %LoopLatchBB, label %PhonySuccessorBB
  //  LoopLatchBB: <latch,exiting>    ; preds = %LoopHeaderBB
  //    <...>
  //    br i1 %<...>, label %PhonySuccessorBB, label %LoopHeaderBB
  //  PhonySuccessorBB: <uniq. exit>  ; preds = %LoopHeaderBB, %LoopLatchBB
  //    unreachable
  //  EqualBB:                        ; No preds, UNREACHABLE!
  //    br label %Successor1BB
  //  UnequalBB:                      ; No preds, UNREACHABLE!
  //    br label %Successor0BB
  //  Successor0BB:                   ; preds = %UnequalBB
  //    %S0PHI = phi <...> [ <...>, %UnequalBB ]
  //    <...>
  //  Successor1BB:                   ; preds = %EqualBB
  //    %S0PHI = phi <...> [ <...>, %EqualBB ]
  //    <...>

  // *Finally*, zap the original loop. Record it's parent loop though.
  Loop *ParentLoop = CurLoop->getParentLoop();
  LLVM_DEBUG(dbgs() << "Deleting old loop.\n");
  LoopDeleter.markLoopAsDeleted(CurLoop); // Mark as deleted *BEFORE* deleting!
  deleteDeadLoop(CurLoop, DT, SE, LI);    // And actually delete the loop.
  CurLoop = nullptr;

  // By now we have: (3/6)
  //  PreheaderBB:                    ; preds = ???
  //    <...>
  //    %memcmp = call i32 @memcmp(i8* %LoadSrcA, i8* %LoadSrcB, i64 %Nbytes)
  //    %ComparedEqual = icmp eq <...> %memcmp, 0
  //    [no terminator instruction!]
  //  PhonyPreheaderBB:               ; No preds, UNREACHABLE!
  //    br label %PhonySuccessorBB
  //  PhonySuccessorBB:               ; preds = %PhonyPreheaderBB
  //    unreachable
  //  EqualBB:                        ; No preds, UNREACHABLE!
  //    br label %Successor1BB
  //  UnequalBB:                      ; No preds, UNREACHABLE!
  //    br label %Successor0BB
  //  Successor0BB:                   ; preds = %UnequalBB
  //    %S0PHI = phi <...> [ <...>, %UnequalBB ]
  //    <...>
  //  Successor1BB:                   ; preds = %EqualBB
  //    %S0PHI = phi <...> [ <...>, %EqualBB ]
  //    <...>

  // Now, actually restore the CFG.

  // Insert an unconditional branch from an actual preheader basic block to
  // phony preheader basic block.
  IRBuilder<>(PreheaderBB).CreateBr(PhonyPreheaderBB);
  DTUpdates.push_back({DominatorTree::Insert, PhonyPreheaderBB, HeaderBB});
  // Insert proper conditional branch from phony successor basic block to the
  // "dispatch" basic blocks, which were used to preserve incoming values in
  // original loop's successor basic blocks.
  assert(isa<UnreachableInst>(PhonySuccessorBB->getTerminator()) &&
         "Yep, that's the one we created to keep deleteDeadLoop() happy.");
  PhonySuccessorBB->getTerminator()->eraseFromParent();
  {
    IRBuilder<> Builder(PhonySuccessorBB);
    Builder.SetCurrentDebugLocation(ComparedEqual->getDebugLoc());
    Builder.CreateCondBr(ComparedEqual, ComparedEqualBB, ComparedUnequalBB);
  }
  DTUpdates.push_back(
      {DominatorTree::Insert, PhonySuccessorBB, ComparedEqualBB});
  DTUpdates.push_back(
      {DominatorTree::Insert, PhonySuccessorBB, ComparedUnequalBB});

  BasicBlock *DispatchBB = PhonySuccessorBB;
  DispatchBB->setName(LoopName + ".bcmpdispatchbb");

  assert(DTUpdates.size() == 3 && "Update count prediction failed.");
  DTU.applyUpdates(DTUpdates);
  DTUpdates.clear();

  // By now we have: (4/6)
  //  PreheaderBB:                    ; preds = ???
  //    <...>
  //    %memcmp = call i32 @memcmp(i8* %LoadSrcA, i8* %LoadSrcB, i64 %Nbytes)
  //    %ComparedEqual = icmp eq <...> %memcmp, 0
  //    br label %PhonyPreheaderBB
  //  PhonyPreheaderBB:               ; preds = %PreheaderBB
  //    br label %DispatchBB
  //  DispatchBB:                     ; preds = %PhonyPreheaderBB
  //    br i1 %ComparedEqual, label %EqualBB, label %UnequalBB
  //  EqualBB:                        ; preds = %DispatchBB
  //    br label %Successor1BB
  //  UnequalBB:                      ; preds = %DispatchBB
  //    br label %Successor0BB
  //  Successor0BB:                   ; preds = %UnequalBB
  //    %S0PHI = phi <...> [ <...>, %UnequalBB ]
  //    <...>
  //  Successor1BB:                   ; preds = %EqualBB
  //    %S0PHI = phi <...> [ <...>, %EqualBB ]
  //    <...>

  // The basic CFG has been restored! Now let's merge redundant basic blocks.

  // Merge phony successor basic block into it's only predecessor,
  // phony preheader basic block. It is fully pointlessly redundant.
  MergeBasicBlockIntoOnlyPred(DispatchBB, &DTU);

  // By now we have: (5/6)
  //  PreheaderBB:                    ; preds = ???
  //    <...>
  //    %memcmp = call i32 @memcmp(i8* %LoadSrcA, i8* %LoadSrcB, i64 %Nbytes)
  //    %ComparedEqual = icmp eq <...> %memcmp, 0
  //    br label %DispatchBB
  //  DispatchBB:                     ; preds = %PreheaderBB
  //    br i1 %ComparedEqual, label %EqualBB, label %UnequalBB
  //  EqualBB:                        ; preds = %DispatchBB
  //    br label %Successor1BB
  //  UnequalBB:                      ; preds = %DispatchBB
  //    br label %Successor0BB
  //  Successor0BB:                   ; preds = %UnequalBB
  //    %S0PHI = phi <...> [ <...>, %UnequalBB ]
  //    <...>
  //  Successor1BB:                   ; preds = %EqualBB
  //    %S0PHI = phi <...> [ <...>, %EqualBB ]
  //    <...>

  // Was this loop nested?
  if (!ParentLoop) {
    // If the loop was *NOT* nested, then let's also merge phony successor
    // basic block into it's only predecessor, preheader basic block.
    // Also, here we need to update LoopInfo.
    LI->removeBlock(PreheaderBB);
    MergeBasicBlockIntoOnlyPred(DispatchBB, &DTU);

    // By now we have: (6/6)
    //  DispatchBB:                   ; preds = ???
    //    <...>
    //    %memcmp = call i32 @memcmp(i8* %LoadSrcA, i8* %LoadSrcB, i64 %Nbytes)
    //    %ComparedEqual = icmp eq <...> %memcmp, 0
    //    br i1 %ComparedEqual, label %EqualBB, label %UnequalBB
    //  EqualBB:                      ; preds = %DispatchBB
    //    br label %Successor1BB
    //  UnequalBB:                    ; preds = %DispatchBB
    //    br label %Successor0BB
    //  Successor0BB:                 ; preds = %UnequalBB
    //    %S0PHI = phi <...> [ <...>, %UnequalBB ]
    //    <...>
    //  Successor1BB:                 ; preds = %EqualBB
    //    %S0PHI = phi <...> [ <...>, %EqualBB ]
    //    <...>

    return DispatchBB;
  }

  // Otherwise, we need to "preserve" the LoopSimplify form of the deleted loop.
  // To achieve that, we shall keep the preheader basic block (mainly so that
  // the loop header block will be guaranteed to have a predecessor outside of
  // the loop), and create a phony loop with all these new three basic blocks.
  Loop *PhonyLoop = LI->AllocateLoop();
  ParentLoop->addChildLoop(PhonyLoop);
  PhonyLoop->addBasicBlockToLoop(DispatchBB, *LI);
  PhonyLoop->addBasicBlockToLoop(ComparedEqualBB, *LI);
  PhonyLoop->addBasicBlockToLoop(ComparedUnequalBB, *LI);

  // But we only have a preheader basic block, a header basic block block and
  // two exiting basic blocks. For a proper loop we also need a backedge from
  // non-header basic block to header bb.
  // Let's just add a never-taken branch from both of the exiting basic blocks.
  for (BasicBlock *BB : {ComparedEqualBB, ComparedUnequalBB}) {
    BranchInst *OldTerminator = cast<BranchInst>(BB->getTerminator());
    assert(OldTerminator->isUnconditional() && "That's the one we created.");
    BasicBlock *SuccessorBB = OldTerminator->getSuccessor(0);

    IRBuilder<> Builder(OldTerminator);
    Builder.SetCurrentDebugLocation(OldTerminator->getDebugLoc());
    Builder.CreateCondBr(ConstantInt::getTrue(Context), SuccessorBB,
                         DispatchBB);
    OldTerminator->eraseFromParent();
    // Yes, the backedge will never be taken. The control-flow is redundant.
    // If it can be simplified further, other passes will take care.
    DTUpdates.push_back({DominatorTree::Delete, BB, SuccessorBB});
    DTUpdates.push_back({DominatorTree::Insert, BB, SuccessorBB});
    DTUpdates.push_back({DominatorTree::Insert, BB, DispatchBB});
  }
  assert(DTUpdates.size() == 6 && "Update count prediction failed.");
  DTU.applyUpdates(DTUpdates);
  DTUpdates.clear();

  // By now we have: (6/6)
  //  PreheaderBB: <preheader>        ; preds = ???
  //    <...>
  //    %memcmp = call i32 @memcmp(i8* %LoadSrcA, i8* %LoadSrcB, i64 %Nbytes)
  //    %ComparedEqual = icmp eq <...> %memcmp, 0
  //    br label %BCmpDispatchBB
  //  BCmpDispatchBB: <header>        ; preds = %PreheaderBB
  //    br i1 %ComparedEqual, label %EqualBB, label %UnequalBB
  //  EqualBB: <latch,exiting>        ; preds = %BCmpDispatchBB
  //    br i1 %true, label %Successor1BB, label %BCmpDispatchBB
  //  UnequalBB: <latch,exiting>      ; preds = %BCmpDispatchBB
  //    br i1 %true, label %Successor0BB, label %BCmpDispatchBB
  //  Successor0BB:                   ; preds = %UnequalBB
  //    %S0PHI = phi <...> [ <...>, %UnequalBB ]
  //    <...>
  //  Successor1BB:                   ; preds = %EqualBB
  //    %S0PHI = phi <...> [ <...>, %EqualBB ]
  //    <...>

  // Finally fully DONE!
  return DispatchBB;
}

void LoopIdiomRecognize::transformLoopToBCmp(ICmpInst *BCmpInst,
                                             CmpInst *LatchCmpInst,
                                             LoadInst *LoadA, LoadInst *LoadB,
                                             const SCEV *SrcA, const SCEV *SrcB,
                                             const SCEV *NBytes) {
  // We will be inserting before the terminator instruction of preheader block.
  IRBuilder<> Builder(CurLoop->getLoopPreheader()->getTerminator());

  LLVM_DEBUG(dbgs() << "Transforming bcmp loop idiom into a call.\n");
  LLVM_DEBUG(dbgs() << "Emitting new instructions.\n");

  // Expand the SCEV expressions for both sources to compare, and produce value
  // for the byte len (beware of Iterations potentially being a pointer, and
  // account for element size being BCmpTyBytes bytes, which may be not 1 byte)
  Value *PtrA, *PtrB, *Len;
  {
    SCEVExpander SExp(*SE, *DL, "LoopToBCmp");
    SExp.setInsertPoint(&*Builder.GetInsertPoint());

    auto HandlePtr = [&SExp](LoadInst *Load, const SCEV *Src) {
      SExp.SetCurrentDebugLocation(DebugLoc());
      // If the pointer operand of original load had dbgloc - use it.
      if (const auto *I = dyn_cast<Instruction>(Load->getPointerOperand()))
        SExp.SetCurrentDebugLocation(I->getDebugLoc());
      return SExp.expandCodeFor(Src);
    };
    PtrA = HandlePtr(LoadA, SrcA);
    PtrB = HandlePtr(LoadB, SrcB);

    // For len calculation let's use dbgloc for the loop's latch condition.
    Builder.SetCurrentDebugLocation(LatchCmpInst->getDebugLoc());
    SExp.SetCurrentDebugLocation(LatchCmpInst->getDebugLoc());
    Len = SExp.expandCodeFor(NBytes);

    Type *CmpFuncSizeTy = DL->getIntPtrType(Builder.getContext());
    assert(SE->getTypeSizeInBits(Len->getType()) ==
               DL->getTypeSizeInBits(CmpFuncSizeTy) &&
           "Len should already have the correct size.");

    // Make sure that iteration count is a number, insert ptrtoint cast if not.
    if (Len->getType()->isPointerTy())
      Len = Builder.CreatePtrToInt(Len, CmpFuncSizeTy);
    assert(Len->getType() == CmpFuncSizeTy && "Should have correct type now.");

    Len->setName(Len->getName() + ".bytecount");

    // There is no legality check needed. We want to compare that the memory
    // regions [PtrA, PtrA+Len) and [PtrB, PtrB+Len) are fully identical, equal.
    // For them to be fully equal, they must match bit-by-bit. And likewise,
    // for them to *NOT* be fully equal, they have to differ just by one bit.
    // The step of comparison (bits compared at once) simply does not matter.
  }

  // For the rest of new instructions, dbgloc should point at the value cmp.
  Builder.SetCurrentDebugLocation(BCmpInst->getDebugLoc());

  // Emit the comparison itself.
  auto *CmpCall =
      cast<CallInst>(HasBCmp ? emitBCmp(PtrA, PtrB, Len, Builder, *DL, TLI)
                             : emitMemCmp(PtrA, PtrB, Len, Builder, *DL, TLI));
  // FIXME: add {B,Mem}CmpInst with MemoryCompareInst
  //        (based on MemIntrinsicBase) as base?
  // FIXME: propagate metadata from loads? (alignments, AS, TBAA, ...)

  // {b,mem}cmp returned 0 if they were equal, or non-zero if not equal.
  auto *ComparedEqual = cast<ICmpInst>(Builder.CreateICmpEQ(
      CmpCall, ConstantInt::get(CmpCall->getType(), 0),
      PtrA->getName() + ".vs." + PtrB->getName() + ".eqcmp"));

  BasicBlock *BB = transformBCmpControlFlow(ComparedEqual);
  Builder.ClearInsertionPoint();

  // We're done.
  LLVM_DEBUG(dbgs() << "Transformed loop bcmp idiom into a call.\n");
  ORE.emit([&]() {
    return OptimizationRemark(DEBUG_TYPE, "TransformedBCmpIdiomToCall",
                              CmpCall->getDebugLoc(), BB)
           << "Transformed bcmp idiom into a call to "
           << ore::NV("NewFunction", CmpCall->getCalledFunction())
           << "() function";
  });
  ++NumBCmp;
}

/// Recognizes a bcmp idiom in a non-countable loop.
///
/// If detected, transforms the relevant code to issue the bcmp (or memcmp)
/// intrinsic function call, and returns true; otherwise, returns false.
bool LoopIdiomRecognize::recognizeBCmp() {
  if (!HasMemCmp && !HasBCmp)
    return false;

  ICmpInst *BCmpInst;
  CmpInst *LatchCmpInst;
  LoadInst *LoadA, *LoadB;
  const SCEV *SrcA, *SrcB, *NBytes;
  if (!detectBCmpIdiom(BCmpInst, LatchCmpInst, LoadA, LoadB, SrcA, SrcB,
                       NBytes)) {
    LLVM_DEBUG(dbgs() << "bcmp idiom recognition failed.\n");
    return false;
  }

  transformLoopToBCmp(BCmpInst, LatchCmpInst, LoadA, LoadB, SrcA, SrcB, NBytes);
  return true;
}
