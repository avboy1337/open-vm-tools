/*********************************************************
 * Copyright (C) 2011-2022 VMware, Inc. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published
 * by the Free Software Foundation version 2.1 and no later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE.  See the Lesser GNU General Public
 * License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin St, Fifth Floor, Boston, MA  02110-1301 USA.
 *
 *********************************************************/

/*
 * hostinfoHV.c --
 *
 *    Code to detect different hypervisors and features.
 */

#include <string.h>
#include "vmware.h"
#if defined(__i386__) || defined(__x86_64__)
#  include "x86cpuid_asm.h"
#endif
#if defined __i386__ || defined __x86_64__ || defined __aarch64__
#  include "backdoor_def.h"
#  include "backdoor_types.h"
#endif
#include "hostinfo.h"
#include "util.h"

#define LGPFX "HOSTINFO:"

#define LOGLEVEL_MODULE hostinfo
#include "loglevel_user.h"

#if !defined(_WIN32)

/*
 * Are vmcall/vmmcall available in the compiler?
 */
#if defined(__linux__) && defined(__GNUC__)
#define GCC_VERSION (__GNUC__ * 10000 + \
                     __GNUC_MINOR__ * 100 + \
                     __GNUC_PATCHLEVEL__)
#if GCC_VERSION > 40803 && !defined(__aarch64__)
#define USE_HYPERCALL
#endif
#endif

#define BDOOR_FLAGS_LB_READ (BDOOR_FLAGS_LB | BDOOR_FLAGS_READ)

#define Vmcall(cmd, result)              \
   __asm__ __volatile__(                 \
      "vmcall"                           \
      : "=a" (result)                    \
      : "0"  (BDOOR_MAGIC),              \
        "c"  (cmd),                      \
        "d"  (BDOOR_FLAGS_LB_READ)       \
   )

#define Vmmcall(cmd, result)             \
   __asm__ __volatile__(                 \
      "vmmcall"                          \
      : "=a" (result)                    \
      : "0"  (BDOOR_MAGIC),              \
        "c"  (cmd),                      \
        "d"  (BDOOR_FLAGS_LB_READ)       \
   )

#define Ioportcall(cmd, result)          \
   __asm__ __volatile__(                 \
      "inl %%dx, %%eax"                  \
      : "=a" (result)                    \
      : "0"  (BDOOR_MAGIC),              \
        "c"  (cmd),                      \
        "d"  (BDOOR_PORT)                \
   )
#endif

/*
 *----------------------------------------------------------------------
 *
 *  HostinfoBackdoorGetInterface --
 *
 *      Check whether hypercall is present or backdoor is being used.
 *
 * Results:
 *      BACKDOOR_INTERFACE_VMCALL  - Intel hypercall is used.
 *      BACKDOOR_INTERFACE_VMMCALL - AMD hypercall is used.
 *      BACKDOOR_INTERFACE_IO      - Backdoor I/O Port is used.
 *
 * Side effects:
 *      None
 *
 *----------------------------------------------------------------------
 */
#if defined(__i386__) || defined(__x86_64__)
static BackdoorInterface
HostinfoBackdoorGetInterface(void)
{
#if defined(USE_HYPERCALL)
   /* Setting 'interface' is idempotent, no atomic access is required. */
   static BackdoorInterface interface = BACKDOOR_INTERFACE_NONE;

   if (UNLIKELY(interface == BACKDOOR_INTERFACE_NONE)) {
#if defined(__i386__) || defined(__x86_64__)
      CPUIDRegs regs;

      /* Check whether we're on a VMware hypervisor that supports vmmcall. */
      __GET_CPUID(CPUID_FEATURE_INFORMATION, &regs);
      if (CPUID_ISSET(CPUID_FEATURE_INFORMATION, ECX, HYPERVISOR, regs.ecx)) {
         __GET_CPUID(CPUID_HYPERVISOR_LEVEL_0, &regs);
         if (CPUID_IsRawVendor(&regs, CPUID_VMWARE_HYPERVISOR_VENDOR_STRING)) {
            if (__GET_EAX_FROM_CPUID(CPUID_HYPERVISOR_LEVEL_0) >=
                                     CPUID_VMW_FEATURES) {
               uint32 features = __GET_ECX_FROM_CPUID(CPUID_VMW_FEATURES);
               if (CPUID_ISSET(CPUID_VMW_FEATURES, ECX,
                               VMCALL_BACKDOOR, features)) {
                  interface = BACKDOOR_INTERFACE_VMCALL;
               } else if (CPUID_ISSET(CPUID_VMW_FEATURES, ECX,
                                      VMMCALL_BACKDOOR, features)) {
                  interface = BACKDOOR_INTERFACE_VMMCALL;
               }
            }
         }
      }
      if (interface == BACKDOOR_INTERFACE_NONE) {
         interface = BACKDOOR_INTERFACE_IO;
      }
#else
      interface = BACKDOOR_INTERFACE_IO;
#endif
   }
   return interface;
#else
   return BACKDOOR_INTERFACE_IO;
#endif
}
#endif // defined(__i386__) || defined(__x86_64__)

/*
 *----------------------------------------------------------------------
 *
 *  Hostinfo_HypervisorPresent --
 *
 *      Check if hypervisor is present.
 *
 * Results:
 *      TRUE iff hypervisor cpuid bit is present.
 *
 * Side effects:
 *      None
 *
 *----------------------------------------------------------------------
 */

#if defined(__i386__) || defined(__x86_64__)
static Bool
Hostinfo_HypervisorPresent(void)
{
   static Bool hypervisorPresent;
   CPUIDRegs regs;

   if (!hypervisorPresent) {
      __GET_CPUID(CPUID_FEATURE_INFORMATION, &regs);
      hypervisorPresent = CPUID_ISSET(CPUID_FEATURE_INFORMATION, ECX,
                                      HYPERVISOR, regs.ecx);
   }
   return hypervisorPresent;
}
#endif


/*
 *----------------------------------------------------------------------
 *
 *  Hostinfo_HypervisorCPUIDSig --
 *
 *      Get the hypervisor signature string from CPUID.
 *
 * Results:
 *      Unqualified 16 byte nul-terminated hypervisor string
 *	String may contain garbage and caller must free
 *
 * Side effects:
 *	None
 *
 *----------------------------------------------------------------------
 */

char *
Hostinfo_HypervisorCPUIDSig(void)
{
   uint32 *name = NULL;
#if defined(__i386__) || defined(__x86_64__)
   CPUIDRegs regs;

   if (!Hostinfo_HypervisorPresent()) {
      return NULL;
   }

   __GET_CPUID(0x40000000, &regs);
   if (regs.eax < 0x40000000) {
      Log(LGPFX" CPUID hypervisor bit is set, but no "
          "hypervisor vendor signature is present.\n");
   }

   name = Util_SafeMalloc(4 * sizeof *name);

   name[0] = regs.ebx;
   name[1] = regs.ecx;
   name[2] = regs.edx;
   name[3] = 0;
#endif // defined(__i386__) || defined(__x86_64__)

   return (char *)name;
}


/*
 *----------------------------------------------------------------------
 *
 *  Hostinfo_LogHypervisorCPUID --
 *
 *      Logs hypervisor CPUID leafs.
 *
 * Side effects:
 *      None
 *
 *----------------------------------------------------------------------
 */

void
Hostinfo_LogHypervisorCPUID(void)
{
#if defined(__i386__) || defined(__x86_64__)
   CPUIDRegs regs;
   uint32 maxLeaf;
   uint32 leafId;
   if (!Hostinfo_HypervisorPresent()) {
      Log(LGPFX" Hypervisor not found. CPUID hypervisor bit is not set.\n");
      return;
   }

   __GET_CPUID(0x40000000, &regs);
   maxLeaf = regs.eax > 0x400000FF ? 0x400000FF : regs.eax;
   if (maxLeaf < 0x40000000) {
      Log(LGPFX" CPUID hypervisor bit is set, but no "
          "hypervisor vendor signature is present.\n");
      return;
   } else {
      Log("CPUID level   %10s   %10s   %10s   %10s\n", "EAX", "EBX",
          "ECX", "EDX");
      for (leafId = 0x40000000; leafId <= maxLeaf; leafId++) {
         __GET_CPUID(leafId, &regs);
         Log("0x%08x    0x%08x   0x%08x   0x%08x   0x%08x\n",
             leafId, regs.eax, regs.ebx, regs.ecx, regs.edx);
      }
   }
#endif // defined(__i386__) || defined(__x86_64__)

   return;
}


/*
 *----------------------------------------------------------------------
 *
 *  Hostinfo_HypervisorInterfaceSig --
 *
 *      Get hypervisor interface signature string from CPUID.
 *
 * Results:
 *      Unqualified 8 byte nul-terminated hypervisor interface signature string.
 *      String may contain garbage and caller must free.
 *      NULL if hypervisor is not present.
 *
 * Side effects:
 *      None
 *
 *----------------------------------------------------------------------
 */

char *
Hostinfo_HypervisorInterfaceSig(void)
{
   uint32 *intrSig = NULL;
#if defined(__i386__) || defined(__x86_64__)
   CPUIDRegs regs;

   if (!Hostinfo_HypervisorPresent()) {
      return NULL;
   }

   __GET_CPUID(0x40000000, &regs);
   if (regs.eax < 0x40000001) {
      Log(LGPFX" CPUID hypervisor bit is set, but no "
          "hypervisor interface signature is present.\n");
      return NULL;
   }

   __GET_CPUID(0x40000001, &regs);
   if (regs.eax != 0) {
      intrSig = Util_SafeMalloc(2 * sizeof *intrSig);
      intrSig[0] = regs.eax;
      intrSig[1] = 0;
   }
#endif // defined(__i386__) || defined(__x86_64__)

   return (char *)intrSig;
}


/*
 *----------------------------------------------------------------------
 *
 *  Hostinfo_TouchXen --
 *
 *      Check for Xen.
 *
 *      Official way is to call Hostinfo_HypervisorCPUIDSig(), which
 *         returns a hypervisor string.  This is a secondary check
 *	   that guards against a backdoor failure.  See PR156185,
 *         http://xenbits.xensource.com/xen-unstable.hg?file/6a383beedf83/tools/misc/xen-detect.c
 *      (Canonical way is /proc/xen, but CPUID is better).
 *
 * Results:
 *      TRUE if we are running in a Xen dom0 or domU.
 *      Linux:
 *         Illegal instruction exception on real hardware.
 *         Obscure Xen implementations might return FALSE.
 *      Windows:
 *         FALSE on real hardware.
 *
 * Side effects:
 *	Linux: Will raise exception on native hardware.
 *	Windows: None.
 *
 *----------------------------------------------------------------------
 */

Bool
Hostinfo_TouchXen(void)
{
#if defined(__linux__) && (defined(__i386__) || defined(__x86_64__))
#define XEN_CPUID 0x40000000
   CPUIDRegs regs;
   uint32 name[4];

   /*
    * PV mode: ud2a "xen" cpuid (faults on native hardware).
    * (Only Linux can run PV, so skip others here).
    * Since PV cannot trap CPUID, this is a Xen hook.
    */

   regs.eax = XEN_CPUID;
   __asm__ __volatile__(
      "xchgl %%ebx, %0"  "\n\t"
      "ud2a ; .ascii \"xen\" ; cpuid" "\n\t"
      "xchgl %%ebx, %0"
      : "=&r" (regs.ebx), "=&c" (regs.ecx), "=&d" (regs.edx)
      : "a" (regs.eax)
   );

   name[0] = regs.ebx;
   name[1] = regs.ecx;
   name[2] = regs.edx;
   name[3] = 0;

   if (strcmp(CPUID_XEN_HYPERVISOR_VENDOR_STRING, (const char*)name) == 0) {
      return TRUE;
   }

   /* Passed checks.  But native and anything non-Xen would #UD before here. */
   NOT_TESTED();
   Log("Xen detected but hypervisor unrecognized (Xen variant?)\n");
   Log("CPUID 0x4000 0000: eax=%x ebx=%x ecx=%x edx=%x\n",
       regs.eax, regs.ebx, regs.ecx, regs.edx);
#endif

   return FALSE;
}


/*
 *----------------------------------------------------------------------
 *
 *  Hostinfo_HyperV --
 *
 *      Check for HyperV.
 *
 * Results:
 *      TRUE if we are running in HyperV.
 *      FALSE otherwise.
 *
 *----------------------------------------------------------------------
 */

Bool
Hostinfo_HyperV(void)
{
   Bool hyperVAvailable = FALSE;
#if defined(__i386__) || defined(__x86_64__)
   char *hypervisorSig = Hostinfo_HypervisorCPUIDSig();

   if (hypervisorSig) {
      if (!memcmp(CPUID_HYPERV_HYPERVISOR_VENDOR_STRING, hypervisorSig,
                  sizeof CPUID_HYPERV_HYPERVISOR_VENDOR_STRING)) {
         hyperVAvailable = TRUE;
      }
      free(hypervisorSig);
   }
#endif

   return hyperVAvailable;
}


/*
 *----------------------------------------------------------------------
 *
 *  Hostinfo_SynchronizedVTSCs --
 *
 *      Access the backdoor to determine if the VCPUs' TSCs are
 *      synchronized.
 *
 * Results:
 *      TRUE if the outer VM provides synchronized VTSCs.
 *	FALSE otherwise.
 *
 * Side effects:
 *	Exception if not in a VM, so don't do that!
 *
 *----------------------------------------------------------------------
 */

Bool
Hostinfo_SynchronizedVTSCs(void)
{
#if defined(__i386__) || defined(__x86_64__)
   return Hostinfo_VCPUInfoBackdoor(BDOOR_CMD_VCPU_SYNC_VTSCS);
#else
   return FALSE;
#endif
}


#if defined(_WIN32)

#if defined(_WIN64)
// from touchBackdoorMasm64.asm
void Hostinfo_BackdoorInOut(Backdoor_proto *myBp);
#endif


/*
 *----------------------------------------------------------------------
 *
 *  Hostinfo_TouchBackDoor --
 *
 *      Access the backdoor. This is used to determine if we are
 *      running in a VM or on a physical host. On a physical host
 *      this should generate a GP which we catch and thereby determine
 *      that we are not in a VM. However some OSes do not handle the
 *      GP correctly and the process continues running returning garbage.
 *      In this case we check the EBX register which should be
 *      BDOOR_MAGIC if the IN was handled in a VM. Based on this we
 *      return either TRUE or FALSE.
 *
 * Results:
 *      TRUE if we succesfully accessed the backdoor, FALSE or segfault
 *      if not.
 *
 * Side effects:
 *	Exception if not in a VM.
 *
 *----------------------------------------------------------------------
 */

Bool
Hostinfo_TouchBackDoor(void)
{
   uint32 ebxval;

#if defined(_WIN64)
   Backdoor_proto bp;

   bp.in.ax.quad = BDOOR_MAGIC;
   bp.in.size = ~BDOOR_MAGIC;
   bp.in.cx.quad = BDOOR_CMD_GETVERSION;
   bp.in.dx.quad = BDOOR_PORT;

   Hostinfo_BackdoorInOut(&bp);

   ebxval = bp.out.bx.words.low;
#else // _WIN64
   _asm {
         push edx
         push ecx
         push ebx
         mov ecx, BDOOR_CMD_GETVERSION
         mov ebx, ~BDOOR_MAGIC
         mov eax, BDOOR_MAGIC
         mov dx, BDOOR_PORT
         in eax, dx
         mov ebxval, ebx
         pop ebx
         pop ecx
         pop edx
   }
#endif // _WIN64

   return (ebxval == BDOOR_MAGIC) ? TRUE : FALSE;
}


/*
 *----------------------------------------------------------------------
 *
 *  Hostinfo_TouchVirtualPC --
 *
 *      Access MS Virtual PC's backdoor. This is used to determine if
 *      we are running in a MS Virtual PC or on a physical host.  Works
 *      the same as Hostinfo_TouchBackDoor, except the entry to MS VPC
 *      is an invalid opcode instead or writing to a port.  Since
 *      MS VPC is 32-bit only, the WIN64 path returns FALSE.
 *      See:  See: http://www.codeproject.com/KB/system/VmDetect.aspx
 *
 * Results:
 *      TRUE if we succesfully accessed MS Virtual PC, FALSE or
 *      segfault if not.
 *
 * Side effects:
 *	Exception if not in a VM.
 *
 *----------------------------------------------------------------------
 */

Bool
Hostinfo_TouchVirtualPC(void)
{
#if defined(_WIN64)
   return FALSE; // MS Virtual PC is 32-bit only
#else  // _WIN32
   uint32 ebxval;

   _asm {
      push ebx
      mov  ebx, 0

      mov  eax, 1 // Virtual PC function number

      // execute invalid opcode to call into MS Virtual PC

      __emit 0Fh
      __emit 3Fh
      __emit 07h
      __emit 0Bh

      mov ebxval, ebx
      pop ebx
   }
   return !ebxval; // ebx is zero if inside Virtual PC
#endif
}


/*
 *----------------------------------------------------------------------
 *
 *  Hostinfo_NestingSupported --
 *
 *      Access the backdoor with a nesting control query. This is used
 *      to determine if we are running in a VM that supports nesting.
 *      This function should only be called after determining that the
 *	backdoor is present with Hostinfo_TouchBackdoor().
 *
 * Results:
 *      TRUE if the outer VM supports nesting.
 *      FALSE otherwise.
 *
 * Side effects:
 *      Exception if not in a VM, so don't do that!
 *
 *----------------------------------------------------------------------
 */

Bool
Hostinfo_NestingSupported(void)
{
   uint32 cmd = NESTING_CONTROL_QUERY << 16 | BDOOR_CMD_NESTING_CONTROL;
   uint32 result;

#if defined(_WIN64)
   Backdoor_proto bp;

   bp.in.ax.quad = BDOOR_MAGIC;
   bp.in.cx.quad = cmd;
   bp.in.dx.quad = BDOOR_PORT;

   Hostinfo_BackdoorInOut(&bp);

   result = bp.out.ax.words.low;
#else
   _asm {
         push edx
         push ecx
         mov ecx, cmd
         mov eax, BDOOR_MAGIC
         mov dx, BDOOR_PORT
         in eax, dx
         mov result, eax
         pop ecx
         pop edx
   }
#endif

   if (result >= NESTING_CONTROL_QUERY && result != ~0U) {
      return TRUE;
   }
   return FALSE;
}


/*
 *----------------------------------------------------------------------
 *
 *  Hostinfo_VCPUInfoBackdoor --
 *
 *      Access the backdoor with an VCPU info query. This is used to
 *      determine whether a VCPU supports a particular feature,
 *      determined by 'bit'.  This function should only be called after
 *      determining that the backdoor is present with
 *      Hostinfo_TouchBackdoor().
 *
 * Results:
 *      TRUE if the outer VM supports the feature.
 *	FALSE otherwise.
 *
 * Side effects:
 *	Exception if not in a VM, so don't do that!
 *
 *----------------------------------------------------------------------
 */

Bool
Hostinfo_VCPUInfoBackdoor(unsigned bit)
{
   uint32 cmd = BDOOR_CMD_GET_VCPU_INFO;
   uint32 result;

#if defined(_WIN64)
   Backdoor_proto bp;

   bp.in.ax.quad = BDOOR_MAGIC;
   bp.in.cx.quad = cmd;
   bp.in.dx.quad = BDOOR_PORT;

   Hostinfo_BackdoorInOut(&bp);

   result = bp.out.ax.words.low;
#else
   _asm {
         push edx
         push ecx
         mov ecx, cmd
         mov eax, BDOOR_MAGIC
         mov dx, BDOOR_PORT
         in eax, dx
         mov result, eax
         pop ecx
         pop edx
   }
#endif
   /* If reserved bit is 1, this command wasn't implemented. */
   return (result & (1 << BDOOR_CMD_VCPU_RESERVED)) == 0 &&
          (result & (1 << bit))                     != 0;
}


/*
 *----------------------------------------------------------------------
 *
 *  Hostinfo_GetNestedBuildNum --
 *
 *      Perform a backdoor call to query the build number of the
 *      outer VMware hypervisor.
 *
 * Results:
 *      The build number of the outer VMware hypervisor, or -1 if
 *      the backdoor call is not supported.
 *
 * Side effects:
 *      Exception if not in a VM, so don't do that!
 *
 *----------------------------------------------------------------------
 */

uint32
Hostinfo_GetNestedBuildNum(void)
{
   uint32 cmd = BDOOR_CMD_GETBUILDNUM;
   int32 result;

#if defined(_WIN64)
   Backdoor_proto bp;

   bp.in.ax.quad = BDOOR_MAGIC;
   bp.in.cx.quad = cmd;
   bp.in.dx.quad = BDOOR_PORT;

   Hostinfo_BackdoorInOut(&bp);

   result = bp.out.ax.words.low;
#else
   _asm {
         push edx
         push ecx
         mov ecx, cmd
         mov eax, BDOOR_MAGIC
         mov dx, BDOOR_PORT
         in eax, dx
         mov result, eax
         pop ecx
         pop edx
   }
#endif
   return result;
}

#else // !defined(_WIN32)

/*
 *----------------------------------------------------------------------
 *
 * Hostinfo_TouchBackDoor --
 *
 *      Access the backdoor. This is used to determine if we are
 *      running in a VM or on a physical host. On a physical host
 *      this should generate a GP which we catch and thereby determine
 *      that we are not in a VM. However some OSes do not handle the
 *      GP correctly and the process continues running returning garbage.
 *      In this case we check the EBX register which should be
 *      BDOOR_MAGIC if the IN was handled in a VM. Based on this we
 *      return either TRUE or FALSE.  If hypercall support is present,
 *      return TRUE without touching the backdoor.
 *
 * Results:
 *      TRUE if we have hypercall support or succesfully accessed the
 *      backdoor, FALSE or segfault if not.
 *
 * Side effects:
 *      Exception if not in a VM.
 *
 *----------------------------------------------------------------------
 */

Bool
Hostinfo_TouchBackDoor(void)
{
#if defined(__i386__) || defined(__x86_64__)
   uint32 eax;
   uint32 ebx;
   uint32 ecx;

   switch (HostinfoBackdoorGetInterface()) {
#  if defined(USE_HYPERCALL)
   case BACKDOOR_INTERFACE_VMCALL:  // Fall Through
   case BACKDOOR_INTERFACE_VMMCALL:
      return TRUE;
      break;
#  endif
   default:
      __asm__ __volatile__(
#     if defined __PIC__ && !vm_x86_64 // %ebx is reserved by the compiler.
         "xchgl %%ebx, %1" "\n\t"
         "inl %%dx, %%eax" "\n\t"
         "xchgl %%ebx, %1"
         : "=a" (eax),
           "=&rm" (ebx),
#     else
         "inl %%dx, %%eax"
         : "=a" (eax),
           "=b" (ebx),
#     endif
           "=c" (ecx)
         : "0" (BDOOR_MAGIC),
           "1" (~BDOOR_MAGIC),
           "2" (BDOOR_CMD_GETVERSION),
           "d" (BDOOR_PORT)
      );
      break;
   }
   if (ebx == BDOOR_MAGIC) {
      return TRUE;
   }
#elif defined __aarch64__
   register uint32 w0 asm("w0") = BDOOR_MAGIC;
   register uint32 w1 asm("w1") = ~BDOOR_MAGIC;
   register uint32 w2 asm("w2") = BDOOR_CMD_GETVERSION;
   register uint32 w3 asm("w3") = BDOOR_PORT;
   register uint64 x7 asm("x7") = (uint64)X86_IO_MAGIC << 32 |
                                  X86_IO_W7_WITH |
                                  X86_IO_W7_DIR |
                                  2 << X86_IO_W7_SIZE_SHIFT;
   __asm__ __volatile__(
      "mrs xzr, mdccsr_el0     \n\t"
      : "+r" (w0),
        "+r" (w1),
        "+r" (w2)
      : "r" (w3),
        "r" (x7));

   if (w1 == BDOOR_MAGIC) {
      return TRUE;
   }
#endif

   return FALSE;
}


/*
 *----------------------------------------------------------------------
 *
 *  Hostinfo_TouchVirtualPC --
 *
 *      Access MS Virtual PC's backdoor. This is used to determine if
 *      we are running in a MS Virtual PC or on a physical host.  Works
 *      the same as Hostinfo_TouchBackDoor, except the entry to MS VPC
 *      is an invalid opcode instead or writing to a port.  Since
 *      MS VPC is 32-bit only, the 64-bit path returns FALSE.
 *      See: http://www.codeproject.com/KB/system/VmDetect.aspx
 *
 * Results:
 *      TRUE if we succesfully accessed MS Virtual PC, FALSE or
 *      segfault if not.
 *
 * Side effects:
 *      Exception if not in a VM.
 *
 *----------------------------------------------------------------------
 */

Bool
Hostinfo_TouchVirtualPC(void)
{
#if !defined VM_X86_32
   return FALSE;
#else
   uint32 ebxval;

   __asm__ __volatile__ (
#  if defined __PIC__        // %ebx is reserved by the compiler.
     "xchgl %%ebx, %1" "\n\t"
     ".long 0x0B073F0F" "\n\t"
     "xchgl %%ebx, %1"
     : "=&rm" (ebxval)
     : "a" (1),
       "0" (0)
#  else
     ".long 0x0B073F0F"
     : "=b" (ebxval)
     : "a" (1),
       "b" (0)
#  endif
  );
  return !ebxval; // %%ebx is zero if inside Virtual PC
#endif
}


/*
 *----------------------------------------------------------------------
 *
 *  Hostinfo_NestingSupported --
 *
 *      Access the backdoor with a nesting control query. This is used
 *      to determine if we are running inside a VM that supports nesting.
 *      This function should only be called after determining that the
 *	backdoor is present with Hostinfo_TouchBackdoor().
 *
 * Results:
 *      TRUE if the outer VM supports nesting.
 *	FALSE otherwise.
 *
 * Side effects:
 *	Exception if not in a VM, so don't do that!
 *
 *----------------------------------------------------------------------
 */

Bool
Hostinfo_NestingSupported(void)
{
#if defined(__i386__) || defined(__x86_64__)
   uint32 cmd = NESTING_CONTROL_QUERY << 16 | BDOOR_CMD_NESTING_CONTROL;
   uint32 result;

   switch (HostinfoBackdoorGetInterface()) {
#  if defined(USE_HYPERCALL)
   case BACKDOOR_INTERFACE_VMCALL:
      Vmcall(cmd, result);
      break;
   case BACKDOOR_INTERFACE_VMMCALL:
      Vmmcall(cmd, result);
      break;
#  endif
   default:
      Ioportcall(cmd, result);
      break;
   }

   if (result >= NESTING_CONTROL_QUERY && result != ~0U) {
      return TRUE;
   }
#endif

   return FALSE;
}


/*
 *----------------------------------------------------------------------
 *
 *  Hostinfo_VCPUInfoBackdoor --
 *
 *      Access the backdoor with an VCPU info query. This is used to
 *      determine whether a VCPU supports a particular feature,
 *      determined by 'bit'.  This function should only be called after
 *      determining that the backdoor is present with
 *      Hostinfo_TouchBackdoor().
 *
 * Results:
 *      TRUE if the outer VM supports the feature.
 *	FALSE otherwise.
 *
 * Side effects:
 *	Exception if not in a VM, so don't do that!
 *
 *----------------------------------------------------------------------
 */

Bool
Hostinfo_VCPUInfoBackdoor(unsigned bit)
{
#if defined(__i386__) || defined(__x86_64__)
   uint32 result;
   uint32 cmd = BDOOR_CMD_GET_VCPU_INFO;

   switch (HostinfoBackdoorGetInterface()) {
#  if defined(USE_HYPERCALL)
   case BACKDOOR_INTERFACE_VMCALL:
      Vmcall(cmd, result);
      break;
   case BACKDOOR_INTERFACE_VMMCALL:
      Vmmcall(cmd, result);
      break;
#  endif
   default:
      Ioportcall(cmd, result);
      break;
   }
   /* If reserved bit is 1, this command wasn't implemented. */
   return (result & (1 << BDOOR_CMD_VCPU_RESERVED)) == 0 &&
          (result & (1 << bit))                     != 0;
#endif
   return FALSE;
}


/*
 *----------------------------------------------------------------------
 *
 *  Hostinfo_GetNestedBuildNum --
 *
 *      Perform a backdoor call to query the build number of the
 *      outer VMware hypervisor.
 *
 * Results:
 *      The build number of the outer VMware hypervisor, or -1 if
 *      the backdoor call is not supported.
 *
 * Side effects:
 *      Exception if not in a VM, so don't do that!
 *
 *----------------------------------------------------------------------
 */

uint32
Hostinfo_GetNestedBuildNum(void)
{
#if defined(__i386__) || defined(__x86_64__)
   uint32 result;
   uint32 cmd = BDOOR_CMD_GETBUILDNUM;

   switch (HostinfoBackdoorGetInterface()) {
#  if defined(USE_HYPERCALL)
   case BACKDOOR_INTERFACE_VMCALL:
      Vmcall(cmd, result);
      break;
   case BACKDOOR_INTERFACE_VMMCALL:
      Vmmcall(cmd, result);
      break;
#  endif
   default:
      Ioportcall(cmd, result);
      break;
   }
   return result;
#endif
   return 0;
}
#endif

