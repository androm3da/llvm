//===-- PIC16AsmPrinter.cpp - PIC16 LLVM assembly writer ------------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file contains a printer that converts from our internal representation
// of machine-dependent LLVM code to PIC16 assembly language.
//
//===----------------------------------------------------------------------===//

#define DEBUG_TYPE "asm-printer"
#include "PIC16.h"
#include "PIC16TargetMachine.h"
#include "PIC16ConstantPoolValue.h"
#include "PIC16InstrInfo.h"
#include "llvm/Constants.h"
#include "llvm/DerivedTypes.h"
#include "llvm/Module.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/CodeGen/AsmPrinter.h"
#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/CodeGen/MachineConstantPool.h"
#include "llvm/CodeGen/MachineFrameInfo.h"
#include "llvm/CodeGen/MachineInstr.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/Mangler.h"
#include "llvm/Support/MathExtras.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Target/TargetAsmInfo.h"
#include "llvm/Target/TargetData.h"
#include "llvm/Target/TargetMachine.h"
#include "llvm/Target/TargetOptions.h"
#include <cctype>

using namespace llvm;

STATISTIC(EmittedInsts, "Number of machine instrs printed");

namespace {
  struct VISIBILITY_HIDDEN PIC16AsmPrinter : public AsmPrinter {
    PIC16AsmPrinter(raw_ostream &O, TargetMachine &TM, const TargetAsmInfo *T)
      : AsmPrinter(O, TM, T) {
    }


    /// We name each basic block in a Function with a unique number, so
    /// that we can consistently refer to them later. This is cleared
    /// at the beginning of each call to runOnMachineFunction().
    ///
    typedef std::map<const Value *, unsigned> ValueMapTy;
    ValueMapTy NumberForBB;

    /// Keeps the set of GlobalValues that require non-lazy-pointers for
    /// indirect access.
    std::set<std::string> GVNonLazyPtrs;

    /// Keeps the set of external function GlobalAddresses that the asm
    /// printer should generate stubs for.
    std::set<std::string> FnStubs;

    /// True if asm printer is printing a series of CONSTPOOL_ENTRY.
    bool InCPMode;
    
    virtual const char *getPassName() const {
      return "PIC16 Assembly Printer";
    }

    void printOperand(const MachineInstr *MI, int opNum,
                      const char *Modifier = 0);

    void printSOImmOperand(const MachineInstr *MI, int opNum);

    void printAddrModeOperand(const MachineInstr *MI, int OpNo);

    void printRegisterList(const MachineInstr *MI, int opNum);
    void printCPInstOperand(const MachineInstr *MI, int opNum,
                            const char *Modifier);


    bool printInstruction(const MachineInstr *MI);  // autogenerated.
    void emitFunctionStart(MachineFunction &F);
    bool runOnMachineFunction(MachineFunction &F);
    bool doInitialization(Module &M);
    bool doFinalization(Module &M);

    virtual void EmitMachineConstantPoolValue(MachineConstantPoolValue *MCPV);
    
    void getAnalysisUsage(AnalysisUsage &AU) const;

    public:
    void SwitchToTextSection(const char *NewSection, 
                             const GlobalValue *GV = NULL);    
    void SwitchToDataSection(const char *NewSection, 
                             const GlobalValue *GV = NULL);
    void SwitchToDataOvrSection(const char *NewSection, 
                                const GlobalValue *GV = NULL);
  };
} // end of anonymous namespace

#include "PIC16GenAsmWriter.inc"

/// createPIC16CodePrinterPass - Returns a pass that prints the PIC16
/// assembly code for a MachineFunction to the given output stream,
/// using the given target machine description.  This should work
/// regardless of whether the function is in SSA form.
///
FunctionPass *llvm::createPIC16CodePrinterPass(raw_ostream &o,
                                               PIC16TargetMachine &tm) {
  return new PIC16AsmPrinter(o, tm, tm.getTargetAsmInfo());
}

void PIC16AsmPrinter::getAnalysisUsage(AnalysisUsage &AU) const 
{
  // FIXME: Currently unimplemented.
}


void PIC16AsmPrinter ::
EmitMachineConstantPoolValue(MachineConstantPoolValue *MCPV) 
{
  printDataDirective(MCPV->getType());

  PIC16ConstantPoolValue *ACPV = (PIC16ConstantPoolValue*)MCPV;
  GlobalValue *GV = ACPV->getGV();
  std::string Name = GV ? Mang->getValueName(GV) : TAI->getGlobalPrefix();
  if (!GV)
    Name += ACPV->getSymbol();
  if (ACPV->isNonLazyPointer()) {
    GVNonLazyPtrs.insert(Name);
    O << TAI->getPrivateGlobalPrefix() << Name << "$non_lazy_ptr";
  } else if (ACPV->isStub()) {
    FnStubs.insert(Name);
    O << TAI->getPrivateGlobalPrefix() << Name << "$stub";
  } else {
    O << Name;
  }

  if (ACPV->hasModifier()) O << "(" << ACPV->getModifier() << ")";

  if (ACPV->getPCAdjustment() != 0) {
    O << "-(" << TAI->getPrivateGlobalPrefix() << "PC"
      << utostr(ACPV->getLabelId())
      << "+" << (unsigned)ACPV->getPCAdjustment();

    if (ACPV->mustAddCurrentAddress())
      O << "-.";

    O << ")";
  }
  O << "\n";

  // If the constant pool value is a extern weak symbol, remember to emit
  // the weak reference.
  if (GV && GV->hasExternalWeakLinkage())
    ExtWeakSymbols.insert(GV);
}

/// emitFunctionStart - Emit the directives used by ASM on the start of 
/// functions.
void PIC16AsmPrinter::emitFunctionStart(MachineFunction &MF)
{
  // Print out the label for the function.
  const Function *F = MF.getFunction();
  MachineFrameInfo *FrameInfo = MF.getFrameInfo();
  if (FrameInfo->hasStackObjects()) {           
    int indexBegin = FrameInfo->getObjectIndexBegin();
    int indexEnd = FrameInfo->getObjectIndexEnd();
    while (indexBegin < indexEnd) {
      if (indexBegin == 0)                     
        SwitchToDataOvrSection(F->getParent()->getModuleIdentifier().c_str(),
                                F);
                 
        O << "\t\t" << CurrentFnName << "_" << indexBegin << " " << "RES" 
          << " " << FrameInfo->getObjectSize(indexBegin) << "\n" ;
        indexBegin++;
    }
  }
  SwitchToTextSection(CurrentFnName.c_str(), F);  
  O << "_" << CurrentFnName << ":" ; 
  O << "\n";
}


/// runOnMachineFunction - This uses the printInstruction()
/// method to print assembly for each instruction.
///
bool PIC16AsmPrinter::runOnMachineFunction(MachineFunction &MF) 
{
  SetupMachineFunction(MF);
  O << "\n";

  // What's my mangled name?
  CurrentFnName = Mang->getValueName(MF.getFunction());

  // Emit the function start directives
  emitFunctionStart(MF);

  // Print out code for the function.
  for (MachineFunction::const_iterator I = MF.begin(), E = MF.end();
       I != E; ++I) {
    // Print a label for the basic block.
    if (I != MF.begin()) {
      printBasicBlockLabel(I, true);
      O << '\n';
    }
    for (MachineBasicBlock::const_iterator II = I->begin(), E = I->end();
         II != E; ++II) {
      // Print the assembly for the instruction.
      O << '\t';
      printInstruction(II);
      ++EmittedInsts;
    }
  }

  // We didn't modify anything.
  return false;
}

void PIC16AsmPrinter::
printOperand(const MachineInstr *MI, int opNum, const char *Modifier) 
{
  const MachineOperand &MO = MI->getOperand(opNum);
  const TargetRegisterInfo  &RI = *TM.getRegisterInfo();

  switch (MO.getType()) {
    case MachineOperand::MO_Register:
      if (TargetRegisterInfo::isPhysicalRegister(MO.getReg()))
        O << RI.get(MO.getReg()).Name;
      else
        assert(0 && "not implemented");
      break;

    case MachineOperand::MO_Immediate: 
      if (!Modifier || strcmp(Modifier, "no_hash") != 0)
        O << "#";
      O << (int)MO.getImm();
      break;

    case MachineOperand::MO_MachineBasicBlock:
      printBasicBlockLabel(MO.getMBB());
      return;

    case MachineOperand::MO_GlobalAddress: 
      O << Mang->getValueName(MO.getGlobal())<<'+'<<MO.getOffset();
      break;

    case MachineOperand::MO_ExternalSymbol: 
      O << MO.getSymbolName();
      break;

    case MachineOperand::MO_ConstantPoolIndex:
      O << TAI->getPrivateGlobalPrefix() << "CPI" << getFunctionNumber()
        << '_' << MO.getIndex();
      break;

    case MachineOperand::MO_FrameIndex:
      O << "_" << CurrentFnName 
        << '+' << MO.getIndex();
      break;

    case MachineOperand::MO_JumpTableIndex:
      O << TAI->getPrivateGlobalPrefix() << "JTI" << getFunctionNumber()
        << '_' << MO.getIndex();
      break;

    default:
      O << "<unknown operand type>"; abort (); 
      break;
  } // end switch.
}

static void 
printSOImm(raw_ostream &O, int64_t V, const TargetAsmInfo *TAI) 
{
  assert(V < (1 << 12) && "Not a valid so_imm value!");
  
  O << (unsigned) V;
}

/// printSOImmOperand - SOImm is 4-bit rotated amount in bits 8-11 with 8-bit
/// immediate in bits 0-7.
void PIC16AsmPrinter::printSOImmOperand(const MachineInstr *MI, int OpNum) 
{
  const MachineOperand &MO = MI->getOperand(OpNum);
  assert(MO.isImmediate() && "Not a valid so_imm value!");
  printSOImm(O, MO.getImm(), TAI);
}


void PIC16AsmPrinter::printAddrModeOperand(const MachineInstr *MI, int Op) 
{
  const MachineOperand &MO1 = MI->getOperand(Op);
  const MachineOperand &MO2 = MI->getOperand(Op+1);

  if (MO2.isFrameIndex ()) {
    printOperand(MI, Op+1);
    return;
  }

  if (!MO1.isRegister()) {   
    // FIXME: This is for CP entries, but isn't right.
    printOperand(MI, Op);
    return;
  }

  // If this is Stack Slot
  if (MO1.isRegister()) {  
    if (strcmp(TM.getRegisterInfo()->get(MO1.getReg()).Name, "SP") == 0) {
      O << CurrentFnName <<"_"<< MO2.getImm();
      return;
    }
    O << "[" << TM.getRegisterInfo()->get(MO1.getReg()).Name;
    O << "+";
    O << MO2.getImm();
    O << "]";
    return;
  }

  O << "[" << TM.getRegisterInfo()->get(MO1.getReg()).Name;
  O << "]";
}


void PIC16AsmPrinter::printRegisterList(const MachineInstr *MI, int opNum) 
{
  O << "{";
  for (unsigned i = opNum, e = MI->getNumOperands(); i != e; ++i) {
    printOperand(MI, i);
    if (i != e-1) O << ", ";
  }
  O << "}";
}

void PIC16AsmPrinter::
printCPInstOperand(const MachineInstr *MI, int OpNo, const char *Modifier) 
{
  assert(Modifier && "This operand only works with a modifier!");

  // There are two aspects to a CONSTANTPOOL_ENTRY operand, the label and the
  // data itself.
  if (!strcmp(Modifier, "label")) {
    unsigned ID = MI->getOperand(OpNo).getImm();
    O << TAI->getPrivateGlobalPrefix() << "CPI" << getFunctionNumber()
      << '_' << ID << ":\n";
  } else {
    assert(!strcmp(Modifier, "cpentry") && "Unknown modifier for CPE");
    unsigned CPI = MI->getOperand(OpNo).getIndex();

    const MachineConstantPoolEntry &MCPE =  // Chasing pointers is fun?
      MI->getParent()->getParent()->getConstantPool()->getConstants()[CPI];
    
    if (MCPE.isMachineConstantPoolEntry())
      EmitMachineConstantPoolValue(MCPE.Val.MachineCPVal);
    else {
      EmitGlobalConstant(MCPE.Val.ConstVal);
      // remember to emit the weak reference
      if (const GlobalValue *GV = dyn_cast<GlobalValue>(MCPE.Val.ConstVal))
        if (GV->hasExternalWeakLinkage())
          ExtWeakSymbols.insert(GV);
    }
  }
}


bool PIC16AsmPrinter::doInitialization(Module &M) 
{
  bool Result = AsmPrinter::doInitialization(M);
  return Result;
}

bool PIC16AsmPrinter::doFinalization(Module &M) 
{
  const TargetData *TD = TM.getTargetData();

  for (Module::const_global_iterator I = M.global_begin(), E = M.global_end();
       I != E; ++I) {
    if (!I->hasInitializer())   // External global require no code
      continue;

    if (EmitSpecialLLVMGlobal(I)) {
      continue;
    }

    std::string name = Mang->getValueName(I);
    Constant *C = I->getInitializer();
    const Type *Ty = C->getType();
    unsigned Size = TD->getABITypeSize(Ty);
    unsigned Align = TD->getPreferredAlignmentLog(I);

    const char *VisibilityDirective = NULL;
    if (I->hasHiddenVisibility())
      VisibilityDirective = TAI->getHiddenDirective();
    else if (I->hasProtectedVisibility())
      VisibilityDirective = TAI->getProtectedDirective();

    if (VisibilityDirective)
      O << VisibilityDirective << name << "\n";

    if (C->isNullValue()) {
      if (I->hasExternalLinkage()) {
        if (const char *Directive = TAI->getZeroFillDirective()) {
          O << "\t.globl\t" << name << "\n";
          O << Directive << "__DATA__, __common, " << name << ", "
            << Size << ", " << Align << "\n";
          continue;
        }
      }

      if (!I->hasSection() &&
          (I->hasInternalLinkage() || I->hasWeakLinkage() ||
           I->hasLinkOnceLinkage() || I->hasCommonLinkage())) {
        if (Size == 0) Size = 1;   // .comm Foo, 0 is undefined, avoid it.
        SwitchToDataSection(M.getModuleIdentifier().c_str(), I);
        if (TAI->getLCOMMDirective() != NULL) {
          if (I->hasInternalLinkage()) {
            O << TAI->getLCOMMDirective() << name << "," << Size;
          } else
            O << TAI->getCOMMDirective()  << name << "," << Size;
        } else {
          if (I->hasInternalLinkage())
            O << "\t.local\t" << name << "\n";

          O << TAI->getCOMMDirective() <<"\t" << name << " " <<"RES"<< " " 
            << Size;
          O << "\n\t\tGLOBAL" <<" "<< name;
          if (TAI->getCOMMDirectiveTakesAlignment())
            O << "," << (TAI->getAlignmentIsInBytes() ? (1 << Align) : Align);
        }
        continue;
      }
    }

    switch (I->getLinkage()) {
    case GlobalValue::AppendingLinkage:
      // FIXME: appending linkage variables should go into a section of
      // their name or something.  For now, just emit them as external.
      // FALL THROUGH

    case GlobalValue::ExternalLinkage:
      O << "\t.globl " << name << "\n";
      // FALL THROUGH

    case GlobalValue::InternalLinkage: 
      break;

    default:
      assert(0 && "Unknown linkage type!");
      break;
    } // end switch.

    EmitAlignment(Align, I);
    O << name << ":\t\t\t\t" << TAI->getCommentString() << " " << I->getName()
      << "\n";

    // If the initializer is a extern weak symbol, remember to emit the weak
    // reference!
    if (const GlobalValue *GV = dyn_cast<GlobalValue>(C))
      if (GV->hasExternalWeakLinkage())
        ExtWeakSymbols.insert(GV);

    EmitGlobalConstant(C);
    O << '\n';
  } // end for.

  O << "\n "<< "END";
  return AsmPrinter::doFinalization(M);
}

void PIC16AsmPrinter::
SwitchToTextSection(const char *NewSection, const GlobalValue *GV)
{
  O << "\n";
  if (NewSection && *NewSection) {
    std::string codeSection = "code_";
    codeSection += NewSection;
    codeSection += " ";
    codeSection += "CODE";
    AsmPrinter::SwitchToTextSection(codeSection.c_str(), GV);
  } 
  else 
    AsmPrinter::SwitchToTextSection(NewSection, GV);
}

void PIC16AsmPrinter::
SwitchToDataSection(const char *NewSection, const GlobalValue *GV)
{
  // Need to append index for page.
  O << "\n";
  if (NewSection && *NewSection) {
    std::string dataSection = "udata_";
    dataSection += NewSection;
    if (dataSection.substr(dataSection.length() - 2).compare(".o") == 0) {
      dataSection = dataSection.substr(0, dataSection.length() - 2);
    }
    dataSection += " ";
    dataSection += "UDATA";
    AsmPrinter::SwitchToDataSection(dataSection.c_str(), GV);
  } 
  else
    AsmPrinter::SwitchToDataSection(NewSection, GV);
}

void PIC16AsmPrinter::
SwitchToDataOvrSection(const char *NewSection, const GlobalValue *GV)
{
  O << "\n";
  if (NewSection && *NewSection) {
    std::string dataSection = "frame_";
    dataSection += NewSection;
    if (dataSection.substr(dataSection.length() - 2).compare(".o") == 0) {
      dataSection = dataSection.substr(0, dataSection.length() - 2);
    }          
    dataSection += "_";
    dataSection += CurrentFnName;
    dataSection += " ";
    dataSection += "UDATA_OVR";
    AsmPrinter::SwitchToDataSection(dataSection.c_str(), GV);
  } 
  else
    AsmPrinter::SwitchToDataSection(NewSection, GV);
}
