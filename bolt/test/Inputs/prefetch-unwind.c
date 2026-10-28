// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// Validate asynchronous unwinding before one instruction, then replay it.
#define _GNU_SOURCE
#include <dlfcn.h>
#include <signal.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <ucontext.h>
#include <unistd.h>
#include <unwind.h>

typedef uint64_t (*LoopFunction)(uint64_t *, uint64_t, uint64_t);
static uintptr_t TrapPC;
static uint32_t Original;
static void *CodePage;
static size_t Page;
static volatile sig_atomic_t Traps, SeenTrap, SeenCaller;

static _Unwind_Reason_Code trace(struct _Unwind_Context *Context,
                                 void *Unused) {
  uintptr_t PC = _Unwind_GetIP(Context);
  Dl_info Info;
  SeenTrap |= PC == TrapPC;
  if (dladdr((void *)PC, &Info) && Info.dli_sname)
    SeenCaller |= strcmp(Info.dli_sname, "invoke") == 0;
  return _URC_NO_REASON;
}

static void handler(int Signal, siginfo_t *Info, void *Context) {
  ucontext_t *State = Context;
  if (State->uc_mcontext.pc != TrapPC)
    _exit(1);
  _Unwind_Backtrace(trace, NULL);
  ++Traps;
  if (mprotect(CodePage, Page, PROT_READ | PROT_WRITE))
    _exit(1);
  *(uint32_t *)TrapPC = Original;
  __builtin___clear_cache((char *)TrapPC, (char *)(TrapPC + 4));
  if (mprotect(CodePage, Page, PROT_READ | PROT_EXEC))
    _exit(1);
  // Retry the original instruction, including stack updates at spill
  // boundaries.
}

__attribute__((noinline)) uint64_t invoke(LoopFunction Function,
                                          uint64_t *Data) {
  return Function(Data, 0, 3) + 1;
}

int main(int Argc, char **Argv) {
  if (Argc != 5)
    return 1;
  void *Library = dlopen(Argv[1], RTLD_NOW);
  LoopFunction Function = Library ? dlsym(Library, Argv[2]) : NULL;
  if (!Function)
    return 1;
  TrapPC = (uintptr_t)Function + strtoul(Argv[3], NULL, 10);
  uint32_t *Instruction = (uint32_t *)TrapPC;
  Original = *Instruction;
  Page = sysconf(_SC_PAGESIZE);
  CodePage = (void *)(TrapPC & ~(uintptr_t)(Page - 1));
  if (mprotect(CodePage, Page, PROT_READ | PROT_WRITE))
    return 1;
  *Instruction =
      0xd4200000; // Restore and replay this instruction in the handler.
  __builtin___clear_cache((char *)Instruction, (char *)(Instruction + 1));
  if (mprotect(CodePage, Page, PROT_READ | PROT_EXEC))
    return 1;
  struct sigaction Action = {.sa_sigaction = handler, .sa_flags = SA_SIGINFO};
  sigemptyset(&Action.sa_mask);
  if (sigaction(SIGTRAP, &Action, NULL))
    return 1;
  uint64_t Data[12];
  for (unsigned I = 0; I < 12; ++I)
    Data[I] = I + 1;
  return invoke(Function, Data) != strtoul(Argv[4], NULL, 10) + 1 ||
         Traps != 1 || !SeenTrap || !SeenCaller;
}
