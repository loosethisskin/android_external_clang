//===--- CodeGenFunction.cpp - Emit LLVM Code from ASTs for a Function ----===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This coordinates the per-function state used while generating code.
//
//===----------------------------------------------------------------------===//

#include "CodeGenFunction.h"
#include "CodeGenModule.h"
#include "clang/Basic/TargetInfo.h"
#include "clang/AST/AST.h"
#include "llvm/Constants.h"
#include "llvm/DerivedTypes.h"
#include "llvm/Function.h"
#include "llvm/ParamAttrsList.h"
#include "llvm/Analysis/Verifier.h"
#include "llvm/Support/CFG.h"
using namespace clang;
using namespace CodeGen;

CodeGenFunction::CodeGenFunction(CodeGenModule &cgm) 
  : CGM(cgm), Target(CGM.getContext().Target), SwitchInsn(NULL), 
    CaseRangeBlock(NULL) {}

ASTContext &CodeGenFunction::getContext() const {
  return CGM.getContext();
}


llvm::BasicBlock *CodeGenFunction::getBasicBlockForLabel(const LabelStmt *S) {
  llvm::BasicBlock *&BB = LabelMap[S];
  if (BB) return BB;
  
  // Create, but don't insert, the new block.
  return BB = new llvm::BasicBlock(S->getName());
}

llvm::Constant *
CodeGenFunction::GetAddrOfStaticLocalVar(const BlockVarDecl *BVD) {
  return cast<llvm::Constant>(LocalDeclMap[BVD]);
}

const llvm::Type *CodeGenFunction::ConvertType(QualType T) {
  return CGM.getTypes().ConvertType(T);
}

bool CodeGenFunction::hasAggregateLLVMType(QualType T) {
  return !T->isRealType() && !T->isPointerType() && !T->isReferenceType() &&
         !T->isVoidType() && !T->isVectorType() && !T->isFunctionType();
}


void CodeGenFunction::GenerateCode(const FunctionDecl *FD) {
  LLVMIntTy = ConvertType(getContext().IntTy);
  LLVMPointerWidth = static_cast<unsigned>(
    getContext().getTypeSize(getContext().getPointerType(getContext().VoidTy),
                             SourceLocation()));
  
  CurFuncDecl = FD;
  CurFn = cast<llvm::Function>(CGM.GetAddrOfFunctionDecl(FD, true));
  assert(CurFn->isDeclaration() && "Function already has body?");
  
  // TODO: Set up linkage and many other things.  Note, this is a simple 
  // approximation of what we really want.
  if (FD->getAttr<DLLImportAttr>())
    CurFn->setLinkage(llvm::Function::DLLImportLinkage);
  else if (FD->getAttr<DLLExportAttr>())
    CurFn->setLinkage(llvm::Function::DLLExportLinkage);
  else if (FD->getAttr<WeakAttr>() || FD->isInline())
    CurFn->setLinkage(llvm::Function::WeakLinkage);
  else if (FD->getStorageClass() == FunctionDecl::Static)
    CurFn->setLinkage(llvm::Function::InternalLinkage);

  if (const VisibilityAttr *attr = FD->getAttr<VisibilityAttr>())
    CurFn->setVisibility(attr->getVisibility());
  // FIXME: else handle -fvisibility


  llvm::ParamAttrsVector ParamAttrsVec;

  if (FD->getAttr<NoThrowAttr>())
    ParamAttrsVec.push_back(
     llvm::ParamAttrsWithIndex::get(ParamAttrsVec.size(), llvm::ParamAttr::NoUnwind));
  if (FD->getAttr<NoReturnAttr>())
    ParamAttrsVec.push_back(
     llvm::ParamAttrsWithIndex::get(ParamAttrsVec.size(), llvm::ParamAttr::NoReturn));

  if (!ParamAttrsVec.empty())
    CurFn->setParamAttrs(llvm::ParamAttrsList::get(ParamAttrsVec));


  llvm::BasicBlock *EntryBB = new llvm::BasicBlock("entry", CurFn);
  
  // Create a marker to make it easy to insert allocas into the entryblock
  // later.  Don't create this with the builder, because we don't want it
  // folded.
  llvm::Value *Undef = llvm::UndefValue::get(llvm::Type::Int32Ty);
  AllocaInsertPt = new llvm::BitCastInst(Undef, llvm::Type::Int32Ty, "allocapt",
                                         EntryBB);
  
  Builder.SetInsertPoint(EntryBB);
  
  // Emit allocs for param decls.  Give the LLVM Argument nodes names.
  llvm::Function::arg_iterator AI = CurFn->arg_begin();
  
  // Name the struct return argument.
  if (hasAggregateLLVMType(FD->getResultType())) {
    AI->setName("agg.result");
    ++AI;
  }
  
  for (unsigned i = 0, e = FD->getNumParams(); i != e; ++i, ++AI) {
    assert(AI != CurFn->arg_end() && "Argument mismatch!");
    EmitParmDecl(*FD->getParamDecl(i), AI);
  }
  
  // Emit the function body.
  EmitStmt(FD->getBody());
  
  // Emit a return for code that falls off the end. If insert point
  // is a dummy block with no predecessors then remove the block itself.
  llvm::BasicBlock *BB = Builder.GetInsertBlock();
  if (isDummyBlock(BB))
    BB->eraseFromParent();
  else {
    // FIXME: if this is C++ main, this should return 0.
    if (CurFn->getReturnType() == llvm::Type::VoidTy)
      Builder.CreateRetVoid();
    else
      Builder.CreateRet(llvm::UndefValue::get(CurFn->getReturnType()));
  }
  assert(BreakContinueStack.empty() &&
         "mismatched push/pop in break/continue stack!");
  
  // Remove the AllocaInsertPt instruction, which is just a convenience for us.
  AllocaInsertPt->eraseFromParent();
  AllocaInsertPt = 0;
  
  // Verify that the function is well formed.
  assert(!verifyFunction(*CurFn));
}

/// isDummyBlock - Return true if BB is an empty basic block
/// with no predecessors.
bool CodeGenFunction::isDummyBlock(const llvm::BasicBlock *BB) {
  if (BB->empty() && pred_begin(BB) == pred_end(BB)) 
    return true;
  return false;
}

/// StartBlock - Start new block named N. If insert block is a dummy block
/// then reuse it.
void CodeGenFunction::StartBlock(const char *N) {
  llvm::BasicBlock *BB = Builder.GetInsertBlock();
  if (!isDummyBlock(BB))
    EmitBlock(new llvm::BasicBlock(N));
  else
    BB->setName(N);
}

/// getCGRecordLayout - Return record layout info.
const CGRecordLayout *CodeGenFunction::getCGRecordLayout(CodeGenTypes &CGT,
                                                         QualType Ty) {
  const RecordType *RTy = Ty->getAsRecordType();
  assert (RTy && "Unexpected type. RecordType expected here.");

  return CGT.getCGRecordLayout(RTy->getDecl());
}

/// WarnUnsupported - Print out a warning that codegen doesn't support the
/// specified stmt yet.
void CodeGenFunction::WarnUnsupported(const Stmt *S, const char *Type) {
  CGM.WarnUnsupported(S, Type);
}

