/*
PV Drivers for Windows Xen HVM Domains

Copyright (c) 2014, James Harper
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:
    * Redistributions of source code must retain the above copyright
      notice, this list of conditions and the following disclaimer.
    * Redistributions in binary form must reproduce the above copyright
      notice, this list of conditions and the following disclaimer in the
      documentation and/or other materials provided with the distribution.
    * Neither the name of James Harper nor the
      names of its contributors may be used to endorse or promote products
      derived from this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL JAMES HARPER BE LIABLE FOR ANY
DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#include "xenpci.h"

#if defined(_X86_)

/* Is LOCK MOV CR0 available? */
#define CPUID_ALTMOVCR8	(1UL << 4)
/* Task priority register address */
#define LAPIC_TASKPRI 0xFFFE0080
#define TPR_BYTES   0x80, 0x00, 0xfe, 0xff

extern VOID MoveTprToEax(VOID);
extern VOID MoveTprToEcx(VOID);
extern VOID MoveTprToEdx(VOID);
extern VOID MoveTprToEsi(VOID);
extern VOID PushTpr(VOID);
extern VOID MoveEaxToTpr(VOID);
extern VOID MoveEbxToTpr(VOID);
extern VOID MoveEcxToTpr(VOID);
extern VOID MoveEdxToTpr(VOID);
extern VOID MoveEsiToTpr(VOID);
extern VOID MoveConstToTpr(ULONG new_tpr_value);
extern VOID MoveZeroToTpr(VOID);

static PHYSICAL_ADDRESS lapic_page[MAX_VIRT_CPUS];
static volatile PVOID lapic[MAX_VIRT_CPUS];
static ULONG tpr_cache[MAX_VIRT_CPUS];
#define PATCH_METHOD_LOCK_MOVE_CR0 1
#define PATCH_METHOD_CACHED_TPR    2
static ULONG patch_method;

static ULONG
SaveTprProcValue(ULONG cpu, ULONG value) {
  switch (patch_method) {
  case PATCH_METHOD_LOCK_MOVE_CR0:
  case PATCH_METHOD_CACHED_TPR:
    tpr_cache[cpu] = value;
    break;
  }
  return value;
}

static ULONG
SaveTpr() {
  ULONG cpu = KeGetCurrentProcessorNumber() & 0xff;

  switch (patch_method) {
  case PATCH_METHOD_LOCK_MOVE_CR0:
  case PATCH_METHOD_CACHED_TPR:
    return SaveTprProcValue(cpu, *(PULONG)LAPIC_TASKPRI);
  }
  return 0;
}

/* called with interrupts disabled (via CLI) from an arbitrary location inside HAL.DLL */
static __inline LONG
ApicHighestVector(PULONG bitmap) {
  int i;
  ULONG bit;
  ULONG value;
  for (i = 0; i < 8; i++) {
    value = bitmap[(7 - i) * 4];
    if (value) {
      _BitScanReverse(&bit, value);
      return ((7 - i) << 5) | bit;
    }
  }
  return -1;
}

/* called with interrupts disabled (via CLI) from an arbitrary location inside HAL.DLL */
VOID
WriteTpr(ULONG new_tpr_value) {
  ULONG cpu = KeGetCurrentProcessorNumber() & 0xff;
  
  switch (patch_method) {
  case PATCH_METHOD_LOCK_MOVE_CR0:
    tpr_cache[cpu] = new_tpr_value;
    __asm {
      mov eax, new_tpr_value;
      shr	eax, 4;
      lock mov cr0, eax; /* this is actually mov cr8, eax */
    }
    break;
  case PATCH_METHOD_CACHED_TPR:
    if (new_tpr_value != tpr_cache[cpu]) {
      *(PULONG)LAPIC_TASKPRI = new_tpr_value;
      tpr_cache[cpu] = new_tpr_value;
    }
    break;
  }
}

/* called with interrupts disabled (via CLI) from an arbitrary location inside HAL.DLL */
ULONG
ReadTpr() {
  ULONG cpu = KeGetCurrentProcessorNumber() & 0xff;

  switch (patch_method) {
  case PATCH_METHOD_LOCK_MOVE_CR0:
  case PATCH_METHOD_CACHED_TPR:
    return tpr_cache[cpu];
  default:
    return 0;
  }
}

static __inline VOID
InsertCallRel32(PUCHAR address, ULONG target) {
  *address = 0xE8; /* call near */
  *(PULONG)(address + 1) = (ULONG)target - ((ULONG)address + 5);
}

#define PATCH_SIZE 10

typedef struct {
  ULONG patch_type;
  ULONG match_size;
  ULONG function;
  UCHAR bytes[PATCH_SIZE];
} patch_t;

#define PATCH_NONE  0
#define PATCH_1B4   1 /* 1 byte opcode with 4 bytes of data  - replace with call function */
#define PATCH_2B4   2 /* 2 byte opcode with 4 bytes of data - replace with nop + call function*/
#define PATCH_2B5   3 /* 2 byte opcode with 1 + 4 bytes of data - replace with nop + nop + call function */
#define PATCH_2B8   4 /* 2 byte opcode with 4 + 4 bytes of data - replace with push const + call function*/

static patch_t patches[] = {
  { PATCH_1B4,  5, (ULONG)MoveTprToEax,   { 0xa1, TPR_BYTES } },
  { PATCH_2B4,  6, (ULONG)MoveTprToEcx,   { 0x8b, 0x0d, TPR_BYTES } },
  { PATCH_2B4,  6, (ULONG)MoveTprToEdx,   { 0x8b, 0x15, TPR_BYTES } },
  { PATCH_2B4,  6, (ULONG)MoveTprToEsi,   { 0x8b, 0x35, TPR_BYTES } },
  { PATCH_2B4,  6, (ULONG)PushTpr,        { 0xff, 0x35, TPR_BYTES } },
  { PATCH_1B4,  5, (ULONG)MoveEaxToTpr,   { 0xa3, TPR_BYTES } },
  { PATCH_2B4,  6, (ULONG)MoveEbxToTpr,   { 0x89, 0x1D, TPR_BYTES } },
  { PATCH_2B4,  6, (ULONG)MoveEcxToTpr,   { 0x89, 0x0D, TPR_BYTES } },
  { PATCH_2B4,  6, (ULONG)MoveEdxToTpr,   { 0x89, 0x15, TPR_BYTES } },
  { PATCH_2B4,  6, (ULONG)MoveEsiToTpr,   { 0x89, 0x35, TPR_BYTES } },
  { PATCH_2B8,  6, (ULONG)MoveConstToTpr, { 0xC7, 0x05, TPR_BYTES } }, /* + another 4 bytes of const */
  { PATCH_2B5,  7, (ULONG)MoveZeroToTpr,  { 0x83, 0x25, TPR_BYTES, 0 } },
  { PATCH_NONE, 0, 0,                     { 0 } }
};

static BOOLEAN
XenPci_TestAndPatchInstruction(PVOID address) {
  PUCHAR instruction = address;
  ULONG i;
  /* don't declare patches[] on the stack - windows gets grumpy if we allocate too much space on the stack at HIGH_LEVEL */
  
  for (i = 0; patches[i].patch_type != PATCH_NONE; i++) {
    if (memcmp(address, patches[i].bytes, patches[i].match_size) == 0)
      break;
  }

  switch (patches[i].patch_type) {
  case PATCH_1B4:
    InsertCallRel32(instruction + 0, patches[i].function);
    break;
  case PATCH_2B4:
    *(instruction + 0) = 0x90; /* nop */
    InsertCallRel32(instruction + 1, patches[i].function);
    break;
  case PATCH_2B8:
    *(instruction + 0) = 0x68; /* push value */
    *(PULONG)(instruction + 1) = *(PULONG)(instruction + 6);
    InsertCallRel32(instruction + 5, patches[i].function);
    break;
  case PATCH_2B5:
    *(instruction + 0) = 0x90; /* nop */
    *(instruction + 1) = 0x90; /* nop */
    InsertCallRel32(instruction + 2, patches[i].function);
    break;
  default:
    return FALSE;
  }
  return TRUE;
}

typedef struct {
  PVOID base;
  ULONG length;
} patch_info_t;

static PVOID patch_positions[256];
static PVOID potential_patch_positions[256];

static VOID
XenPci_DoPatchKernel0(PVOID context) {
  patch_info_t *pi = context;
  ULONG i;
  ULONG high_level_tpr;
  ULONG patch_position_index = 0;
  ULONG potential_patch_position_index = 0;

  FUNCTION_ENTER();

  high_level_tpr = SaveTpr();
  /* we know all the other CPUs are at HIGH_LEVEL so set them all to the same as cpu 0 */
  for (i = 1; i < MAX_VIRT_CPUS; i++)
    SaveTprProcValue(i, high_level_tpr);

  /* we can't use KdPrint while patching as it may involve the TPR while we are patching it */
  for (i = 0; i < pi->length; i++) {
    if (XenPci_TestAndPatchInstruction((PUCHAR)pi->base + i)) {
      patch_positions[patch_position_index++] = (PUCHAR)pi->base + i;
    } else if (*(PULONG)((PUCHAR)pi->base + i) == LAPIC_TASKPRI) {
      potential_patch_positions[potential_patch_position_index++] = (PUCHAR)pi->base + i;
    }
  }

  for (i = 0; i < patch_position_index; i++)
    FUNCTION_MSG("Patch added at %p\n", patch_positions[i]);

  for (i = 0; i < potential_patch_position_index; i++)
    FUNCTION_MSG("Unpatch TPR address found at %p\n", potential_patch_positions[i]);

  FUNCTION_EXIT();
}

static VOID
XenPci_DoPatchKernelN(PVOID context) {
  UNREFERENCED_PARAMETER(context);
  
  FUNCTION_ENTER();

  FUNCTION_EXIT();
}

static BOOLEAN
IsMoveCr8Supported() {
  DWORD32 cpuid_output[4];
  
  __cpuid(cpuid_output, 0x80000001UL);
  if (cpuid_output[2] & CPUID_ALTMOVCR8)
    return TRUE;
  else
    return FALSE;
}

VOID
XenPci_PatchKernel(PXENPCI_DEVICE_DATA xpdd, PVOID base, ULONG length) {
  patch_info_t patch_info;
#if (NTDDI_VERSION >= NTDDI_WINXP)
  RTL_OSVERSIONINFOEXW version_info;
#endif

  FUNCTION_ENTER();

/* if we're compiled for 2000 then assume we need patching */
#if (NTDDI_VERSION >= NTDDI_WINXP)
  version_info.dwOSVersionInfoSize = sizeof(RTL_OSVERSIONINFOEXW);

  RtlGetVersion((PRTL_OSVERSIONINFOW)&version_info);
  if (version_info.dwMajorVersion >= 6) {
    FUNCTION_MSG("Vista or newer - no need for patch\n");
    return;
  }
  if (version_info.dwMajorVersion == 5
      && version_info.dwMinorVersion > 2) {
    FUNCTION_MSG("Windows 2003 sp2 or newer - no need for patch\n");
    return;
  }
  if (version_info.dwMajorVersion == 5
      && version_info.dwMinorVersion == 2
      && version_info.wServicePackMajor >= 2) {
    FUNCTION_MSG("Windows 2003 sp2 or newer - no need for patch\n");
    return;
  }
#endif
  if (IsMoveCr8Supported()) {
    FUNCTION_MSG("Using LOCK MOVE CR0 TPR patch\n");
    patch_method = PATCH_METHOD_LOCK_MOVE_CR0;
  } else {
    FUNCTION_MSG("Using cached TPR patch\n");
    patch_method = PATCH_METHOD_CACHED_TPR;
  }
  patch_info.base = base;
  patch_info.length = length;
    
  XenPci_HighSync(XenPci_DoPatchKernel0, XenPci_DoPatchKernelN, &patch_info);
  
  xpdd->removable = FALSE;
  
  FUNCTION_EXIT();
}

#else

VOID
XenPci_PatchKernel(PXENPCI_DEVICE_DATA xpdd, PVOID base, ULONG length) {
  UNREFERENCED_PARAMETER(xpdd);
  UNREFERENCED_PARAMETER(base);
  UNREFERENCED_PARAMETER(length);
}

#endif
