//===- DumpInterferenceGraph.cpp - Dump reg alloc interference graph ------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "llvm/CodeGen/LiveIntervals.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/CodeGen/MachineRegisterInfo.h"
#include "llvm/CodeGen/Passes.h"
#include "llvm/CodeGen/TargetRegisterInfo.h"
#include "llvm/InitializePasses.h"
#include "llvm/Pass.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/raw_ostream.h"

#include <map>
#include <vector>

using namespace llvm;

#define DEBUG_TYPE "dump-interference-graph"

static cl::opt<bool>
    EnableDumpIG("enable-dump-ig", cl::Hidden, cl::init(false),
                 cl::desc("Dump interference graphs in DIMACS format"));

namespace {

class DumpInterferenceGraphLegacy : public MachineFunctionPass {
public:
  static char ID;

  DumpInterferenceGraphLegacy() : MachineFunctionPass(ID) {
    initializeDumpInterferenceGraphLegacyPass(
        *PassRegistry::getPassRegistry());
  }

  void getAnalysisUsage(AnalysisUsage &AU) const override {
    AU.setPreservesAll();
    AU.addRequired<LiveIntervalsWrapperPass>();
    MachineFunctionPass::getAnalysisUsage(AU);
  }

  bool runOnMachineFunction(MachineFunction &MF) override;
};

} // end anonymous namespace

char DumpInterferenceGraphLegacy::ID = 0;

char &llvm::DumpInterferenceGraphID = DumpInterferenceGraphLegacy::ID;

INITIALIZE_PASS_BEGIN(DumpInterferenceGraphLegacy, DEBUG_TYPE,
                      "Dump Interference Graph", false, true)
INITIALIZE_PASS_DEPENDENCY(LiveIntervalsWrapperPass)
INITIALIZE_PASS_END(DumpInterferenceGraphLegacy, DEBUG_TYPE,
                    "Dump Interference Graph", false, true)

bool DumpInterferenceGraphLegacy::runOnMachineFunction(MachineFunction &MF) {
  if (!EnableDumpIG)
    return false;

  LiveIntervals &LIS = getAnalysis<LiveIntervalsWrapperPass>().getLIS();
  const MachineRegisterInfo &MRI = MF.getRegInfo();
  const TargetRegisterInfo *TRI = MRI.getTargetRegisterInfo();

  // Group virtual registers by register class.
  std::map<const TargetRegisterClass *, std::vector<unsigned>> ClassToVRegs;

  for (unsigned i = 0, e = MRI.getNumVirtRegs(); i < e; ++i) {
    Register VReg = Register::index2VirtReg(i);
    if (!LIS.hasInterval(VReg))
      continue;
    const TargetRegisterClass *RC = MRI.getRegClassOrNull(VReg);
    if (!RC)
      continue;
    ClassToVRegs[RC].push_back(i);
  }

  // Create output directory.
  sys::fs::create_directories("./interference_graphs");

  for (auto &[RC, VRegIndices] : ClassToVRegs) {
    unsigned N = VRegIndices.size();
    if (N < 2)
      continue;

    // Build edge list.
    std::vector<std::pair<unsigned, unsigned>> Edges;
    for (unsigned a = 0; a < N; ++a) {
      Register VRegA = Register::index2VirtReg(VRegIndices[a]);
      LiveInterval &LI_A = LIS.getInterval(VRegA);
      for (unsigned b = a + 1; b < N; ++b) {
        Register VRegB = Register::index2VirtReg(VRegIndices[b]);
        LiveInterval &LI_B = LIS.getInterval(VRegB);
        if (LI_A.overlaps(LI_B))
          Edges.emplace_back(a + 1, b + 1); // 1-based for DIMACS
      }
    }

    if (Edges.empty())
      continue;

    // Count physical registers in this class = k for coloring.
    unsigned NumPhysRegs = 0;
    for (MCPhysReg Reg : RC->getRegisters()) {
      (void)Reg;
      ++NumPhysRegs;
    }

    // Write DIMACS file.
    std::string Filename = ("./interference_graphs/" + MF.getName() + "_" +
                            TRI->getRegClassName(RC) + ".dimacs")
                               .str();
    std::error_code EC;
    raw_fd_ostream File(Filename, EC, sys::fs::OF_Text);
    if (EC) {
      errs() << "Error opening " << Filename << ": " << EC.message() << "\n";
      continue;
    }

    File << "c Function: " << MF.getName()
         << ", RegClass: " << TRI->getRegClassName(RC)
         << ", NumPhysRegs: " << NumPhysRegs << "\n";
    File << "p edge " << N << " " << Edges.size() << "\n";
    for (auto &[U, V] : Edges)
      File << "e " << U << " " << V << "\n";
  }

  return false;
}
