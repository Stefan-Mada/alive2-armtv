// include first to avoid ambiguity for comparison operator from
// util/spaceship.h
#include "llvm/MC/MCAsmInfo.h"

#include "backend_tv/lifter.h"
#include "util/sort.h"

#include "llvm/ADT/BitVector.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/SetVector.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Analysis/TargetLibraryInfo.h"
#include "llvm/AsmParser/Parser.h"
#include "llvm/Bitcode/BitcodeReader.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/IR/Module.h"
#include "llvm/IRReader/IRReader.h"
#include "llvm/InitializePasses.h"
#include "llvm/MC/MCContext.h"
#include "llvm/MC/MCExpr.h"
#include "llvm/MC/MCInstBuilder.h"
#include "llvm/MC/MCInstPrinter.h"
#include "llvm/MC/MCInstrAnalysis.h"
#include "llvm/MC/MCInstrInfo.h"
#include "llvm/MC/MCParser/MCAsmParser.h"
#include "llvm/MC/MCParser/MCTargetAsmParser.h"
#include "llvm/MC/MCRegisterInfo.h"
#include "llvm/MC/MCStreamer.h"
#include "llvm/MC/MCSubtargetInfo.h"
#include "llvm/MC/MCSymbol.h"
#include "llvm/MC/MCTargetOptions.h"
#include "llvm/MC/MCTargetOptionsCommandFlags.h"
#include "llvm/MC/TargetRegistry.h"
#include "llvm/Pass.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Support/MathExtras.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/PrettyStackTrace.h"
#include "llvm/Support/Signals.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Target/TargetMachine.h"
#include "llvm/Target/TargetOptions.h"
#include "llvm/TargetParser/Triple.h"
#include "llvm/Transforms/Utils/Cloning.h"

#include <algorithm>
#include <fstream>
#include <iostream>
#include <ranges>
#include <sstream>
#include <unordered_map>
#include <utility>
#include <vector>

using namespace std;
using namespace llvm;
using namespace lifter;

namespace {

/*
 * a function that have the sext or zext attribute on its return value
 * is awkward: this obligates the function to sign- or zero-extend the
 * return value. we want to check this, which requires making the
 * function return a wider type, which requires cloning the function.
 *
 * cloning a function is an awkward case and we'd like to thoroughly
 * test that sort of code, so we just unconditionally do that even
 * when we don't actually need to.
 */
Function *adjustSrcReturn(Function *srcFn) {
  auto *origRetTy = srcFn->getReturnType();

  if (!(origRetTy->isIntegerTy() || origRetTy->isVectorTy() ||
        origRetTy->isPointerTy() || origRetTy->isVoidTy())) {
    *out << "\nERROR: Unsupported Function Return Type: Only int, ptr, vec, "
            "and void supported for now\n\n";
    exit(-1);
  }

  auto &DL = srcFn->getParent()->getDataLayout();
  origRetWidth = origRetTy->isVoidTy() ? 0 : DL.getTypeSizeInBits(origRetTy);

  if (origRetTy->isVectorTy()) {
    if (origRetWidth > 128) {
      *out << "\nERROR: Unsupported Function Return: vector > 128 bits\n\n";
      exit(-1);
    }
  } else {
    if (origRetWidth > 64) {
      *out << "\nERROR: Unsupported Function Return: Only int/vec/ptr types 64 "
              "bits or smaller supported for now\n\n";
      exit(-1);
    }
  }

  has_ret_attr = srcFn->hasRetAttribute(Attribute::SExt) ||
                 srcFn->hasRetAttribute(Attribute::ZExt);

  Type *actualRetTy = nullptr;
  if (has_ret_attr && origRetWidth != 32 && origRetWidth != 64) {
    auto *i32 = Type::getIntNTy(srcFn->getContext(), 32);
    auto *i64 = Type::getIntNTy(srcFn->getContext(), 64);

    // build this first to avoid iterator invalidation
    vector<ReturnInst *> RIs;
    for (auto &BB : *srcFn)
      for (auto &I : BB)
        if (auto *RI = dyn_cast<ReturnInst>(&I))
          RIs.push_back(RI);

    for (auto *RI : RIs) {
      auto retVal = RI->getReturnValue();
      auto Name = retVal->getName();
      if (origRetTy->isVectorTy()) {
        retVal = new BitCastInst(
            retVal, Type::getIntNTy(srcFn->getContext(), origRetWidth),
            Name + "_bitcast", RI);
      }
      if (srcFn->hasRetAttribute(Attribute::ZExt)) {
        retVal = new ZExtInst(retVal, i64, Name + "_zext", RI);
      } else {
        if (origRetWidth < 32) {
          auto sext = new SExtInst(retVal, i32, Name + "_sext", RI);
          retVal = new ZExtInst(sext, i64, Name + "_zext", RI);
        } else {
          retVal = new SExtInst(retVal, i64, Name + "_sext", RI);
        }
      }
      ReturnInst::Create(srcFn->getContext(), retVal, RI);
      RI->eraseFromParent();
    }
    actualRetTy = i64;
  } else {
    actualRetTy = origRetTy;
  }

  FunctionType *NFTy =
      FunctionType::get(actualRetTy, srcFn->getFunctionType()->params(), false);
  Function *NF =
      Function::Create(NFTy, srcFn->getLinkage(), srcFn->getAddressSpace(),
                       srcFn->getName(), srcFn->getParent());
  NF->copyAttributesFrom(srcFn);

  NF->splice(NF->begin(), srcFn);
  NF->takeName(srcFn);
  for (Function::arg_iterator I = srcFn->arg_begin(), E = srcFn->arg_end(),
                              I2 = NF->arg_begin();
       I != E; ++I, ++I2) {
    I->replaceAllUsesWith(&*I2);
  }

  // FIXME -- doesn't matter if we're just dealing with one function,
  // but if we're lifting modules with important calls, we need to
  // replace uses of the function with NF, see code in
  // DeadArgumentElimination.cpp

  srcFn->eraseFromParent();
  return NF;
}

#if 0
void checkTy(Type *t, const DataLayout &DL) {
  if (t->isVoidTy())
    return;
  if (t->isStructTy()) {
    *out << "\nERROR: we don't support structures in arguments yet\n\n";
    exit(-1);
  }
  if (DL.getTypeSizeInBits(t) > 64) {
    *out << "\nERROR: integer arguments can't be more than 64 bits yet\n\n";
    exit(-1);
  }
  if (auto pty = dyn_cast<PointerType>(t)) {
    if (pty->getAddressSpace() != 0) {
      *out << "\nERROR: Unsupported function argument: Only address space "
              "0 is supported\n\n";
      exit(-1);
    }
  } else if (t->isIntegerTy()) {
  } else if (t->isVectorTy()) {
  } else {
    *out << "\nERROR: only integer, pointer, and vector arguments supported so "
            "far\n\n";
    exit(-1);
  }
}
#endif

void checkSupport(Instruction &i, const DataLayout &DL) {
  for (auto &op : i.operands()) {
    auto *ty = op.get()->getType();
    if (auto *pty = dyn_cast<PointerType>(ty)) {
      if (pty->getAddressSpace() != 0) {
        *out << "\nERROR: address spaces other than 0 are unsupported\n\n";
        exit(-1);
      }
    }
  }
  if (auto *gep = dyn_cast<GetElementPtrInst>(&i)) {
    if (!gep->isInBounds()) {
      *out << "\nERROR: only inbounds GEP instructions supported for now\n\n";
      exit(-1);
    }
  }
  if (i.isVolatile()) {
    *out << "\nERROR: volatiles not supported yet\n\n";
    exit(-1);
  }
  if (i.isAtomic()) {
    *out << "\nERROR: atomics not supported yet\n\n";
    exit(-1);
  }
  if (isa<IntToPtrInst>(&i)) {
    *out << "\nERROR: int2ptr instructions not supported yet\n\n";
    exit(-1);
  }
  if (isa<InvokeInst>(&i)) {
    *out << "\nERROR: invoke instructions not supported\n\n";
    exit(-1);
  }
  if (auto *ci = dyn_cast<CallInst>(&i)) {
    if (auto *ii = dyn_cast<IntrinsicInst>(ci)) {
      if (IntrinsicInst::mayLowerToFunctionCall(ii->getIntrinsicID())) {
        *out << "\nERROR: intrinsics that may lower to calls are not supported "
                "yet\n\n";
        exit(-1);
      }
    } else {
      *out << "\nERROR: calls not supported\n\n";
      exit(-1);
    }
    auto callee = (string)ci->getCalledFunction()->getName();
    if (callee.find("llvm.objc") != string::npos) {
      *out << "\nERROR: llvm.objc instrinsics not supported\n\n";
      exit(-1);
    }
    if (callee.find("llvm.thread") != string::npos) {
      *out << "\nERROR: llvm.thread instrinsics not supported\n\n";
      exit(-1);
    }
    if ((callee.find("llvm.experimental.gc") != string::npos) ||
        (callee.find("llvm.experimental.stackmap") != string::npos)) {
      *out << "\nERROR: llvm GC instrinsics not supported\n\n";
      exit(-1);
    }
  }
}

void checkVectorTy(VectorType *Ty) {
  auto *EltTy = Ty->getElementType();
  if (auto *IntTy = dyn_cast<IntegerType>(EltTy)) {
    auto Width = IntTy->getBitWidth();
    if (Width != 8 && Width != 16 && Width != 32 && Width != 64) {
      *out << "\nERROR: Only vectors of i8, i16, i32, i64 are supported\n\n";
      exit(-1);
    }
    auto Count = Ty->getElementCount().getFixedValue();
    auto VecSize = (Count * Width) / 8;
    if (VecSize != 8 && VecSize != 16) {
      *out << "\nERROR: Only short vectors 8 and 16 bytes long are supported, "
              "in parameters and return values; please see Section 5.4 of "
              "AAPCS64 for more details\n\n";
      exit(-1);
    }
  } else {
    *out << "\nERROR: Only vectors of integers supported for now\n\n";
    exit(-1);
  }
}

} // namespace

namespace lifter {

Function *adjustSrc(Function *srcFn) {
  if (srcFn->getCallingConv() != CallingConv::C) {
    *out << "\nERROR: Only the C calling convention is supported\n\n";
    exit(-1);
  }

  if (srcFn->isVarArg()) {
    *out << "\nERROR: varargs not supported yet\n\n";
    exit(-1);
  }

  int i = 0;
  for (auto &v : srcFn->args()) {
    auto *ty = v.getType();
    if (ty->isStructTy()) {
      *out << "\nERROR: we don't support structures in arguments yet\n\n";
      exit(-1);
    }
    if (ty->isFloatTy()) {
      *out << "\nERROR: we don't support floats in arguments yet\n\n";
      exit(-1);
    }
    auto &DL = srcFn->getParent()->getDataLayout();
    auto orig_width = DL.getTypeSizeInBits(ty);
    cout << "size of arg " << i << " = " << orig_width << endl;
    if (ty->isVectorTy()) {
      checkVectorTy(dyn_cast<VectorType>(ty));
      if (orig_width > 128) {
        *out << "\nERROR: Vector arguments >128 bits not supported\n\n";
        exit(-1);
      }
    } else {
      if (orig_width > 64) {
        *out << "\nERROR: Unsupported function argument: Only integer / "
                "pointer parameters 64 bits or smaller supported for now\n\n";
        exit(-1);
      }
    }
    ++i;
  }

  auto RT = dyn_cast<VectorType>(srcFn->getReturnType());
  if (RT)
    checkVectorTy(RT);

  auto &DL = srcFn->getParent()->getDataLayout();
  unsigned llvmInstCount = 0;
  for (auto &bb : *srcFn) {
    for (auto &i : bb) {
      checkSupport(i, DL);
      ++llvmInstCount;
    }
  }

  *out << "source function has " << llvmInstCount << " LLVM instructions\n";

  srcFn = adjustSrcReturn(srcFn);

  *out << "\n---------- src.ll (args/return adjusted) -------\n";
  *out << moduleToString(srcFn->getParent());

  return srcFn;
}

} // namespace lifter
