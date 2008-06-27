/* $Id$ */
/** @file
 * EM - Execution Monitor(/Manager) - All contexts
 */

/*
 * Copyright (C) 2006-2007 Sun Microsystems, Inc.
 *
 * This file is part of VirtualBox Open Source Edition (OSE), as
 * available from http://www.virtualbox.org. This file is free software;
 * you can redistribute it and/or modify it under the terms of the GNU
 * General Public License (GPL) as published by the Free Software
 * Foundation, in version 2 as it comes in the "COPYING" file of the
 * VirtualBox OSE distribution. VirtualBox OSE is distributed in the
 * hope that it will be useful, but WITHOUT ANY WARRANTY of any kind.
 *
 * Please contact Sun Microsystems, Inc., 4150 Network Circle, Santa
 * Clara, CA 95054 USA or visit http://www.sun.com if you need
 * additional information or have any questions.
 */


/*******************************************************************************
*   Header Files                                                               *
*******************************************************************************/
#define LOG_GROUP LOG_GROUP_EM
#include <VBox/em.h>
#include <VBox/mm.h>
#include <VBox/selm.h>
#include <VBox/patm.h>
#include <VBox/csam.h>
#include <VBox/pgm.h>
#include <VBox/iom.h>
#include <VBox/stam.h>
#include "EMInternal.h"
#include <VBox/vm.h>
#include <VBox/hwaccm.h>
#include <VBox/tm.h>
#include <VBox/pdmapi.h>

#include <VBox/param.h>
#include <VBox/err.h>
#include <VBox/dis.h>
#include <VBox/disopcode.h>
#include <VBox/log.h>
#include <iprt/assert.h>
#include <iprt/asm.h>
#include <iprt/string.h>


/*******************************************************************************
*   Structures and Typedefs                                                    *
*******************************************************************************/
typedef DECLCALLBACK(uint32_t) PFN_EMULATE_PARAM2_UINT32(void *pvParam1, uint64_t val2);
typedef DECLCALLBACK(uint32_t) PFN_EMULATE_PARAM2(void *pvParam1, size_t val2);
typedef DECLCALLBACK(uint32_t) PFN_EMULATE_PARAM3(void *pvParam1, uint64_t val2, size_t val3);
typedef DECLCALLBACK(int)      FNEMULATELOCKPARAM2(void *pvParam1, uint64_t val2, RTGCUINTREG32 *pf);
typedef FNEMULATELOCKPARAM2 *PFNEMULATELOCKPARAM2;
typedef DECLCALLBACK(int)      FNEMULATELOCKPARAM3(void *pvParam1, uint64_t val2, size_t cb, RTGCUINTREG32 *pf);
typedef FNEMULATELOCKPARAM3 *PFNEMULATELOCKPARAM3;


/*******************************************************************************
*   Internal Functions                                                         *
*******************************************************************************/
DECLINLINE(int) emInterpretInstructionCPU(PVM pVM, PDISCPUSTATE pCpu, PCPUMCTXCORE pRegFrame, RTGCPTR pvFault, uint32_t *pcbSize);


/**
 * Get the current execution manager status.
 *
 * @returns Current status.
 */
EMDECL(EMSTATE) EMGetState(PVM pVM)
{
    return pVM->em.s.enmState;
}


#ifndef IN_GC
/**
 * Read callback for disassembly function; supports reading bytes that cross a page boundary
 *
 * @returns VBox status code.
 * @param   pSrc        GC source pointer
 * @param   pDest       HC destination pointer
 * @param   cb          Number of bytes to read
 * @param   dwUserdata  Callback specific user data (pCpu)
 *
 */
DECLCALLBACK(int) EMReadBytes(RTUINTPTR pSrc, uint8_t *pDest, unsigned cb, void *pvUserdata)
{
    DISCPUSTATE  *pCpu     = (DISCPUSTATE *)pvUserdata;
    PVM           pVM      = (PVM)pCpu->apvUserData[0];
#ifdef IN_RING0
    int rc = PGMPhysReadGCPtr(pVM, pDest, pSrc, cb);
    AssertRC(rc);
#else
    if (!PATMIsPatchGCAddr(pVM, pSrc))
    {
        int rc = PGMPhysReadGCPtr(pVM, pDest, pSrc, cb);
        AssertRC(rc);
    }
    else
    {
        for (uint32_t i = 0; i < cb; i++)
        {
            uint8_t opcode;
            if (VBOX_SUCCESS(PATMR3QueryOpcode(pVM, (RTGCPTR)pSrc + i, &opcode)))
            {
                *(pDest+i) = opcode;
            }
        }
    }
#endif /* IN_RING0 */
    return VINF_SUCCESS;
}

DECLINLINE(int) emDisCoreOne(PVM pVM, DISCPUSTATE *pCpu, RTGCUINTPTR InstrGC, uint32_t *pOpsize)
{
    return DISCoreOneEx(InstrGC, pCpu->mode, EMReadBytes, pVM, pCpu,  pOpsize);
}

#else

DECLINLINE(int) emDisCoreOne(PVM pVM, DISCPUSTATE *pCpu, RTGCUINTPTR InstrGC, uint32_t *pOpsize)
{
    return DISCoreOne(pCpu, InstrGC, pOpsize);
}

#endif


/**
 * Disassembles one instruction.
 *
 * @param   pVM             The VM handle.
 * @param   pCtxCore        The context core (used for both the mode and instruction).
 * @param   pCpu            Where to return the parsed instruction info.
 * @param   pcbInstr        Where to return the instruction size. (optional)
 */
EMDECL(int) EMInterpretDisasOne(PVM pVM, PCCPUMCTXCORE pCtxCore, PDISCPUSTATE pCpu, unsigned *pcbInstr)
{
    RTGCPTR GCPtrInstr;
    int rc = SELMValidateAndConvertCSAddr(pVM, pCtxCore->eflags, pCtxCore->ss, pCtxCore->cs, (PCPUMSELREGHID)&pCtxCore->csHid, (RTGCPTR)pCtxCore->rip, &GCPtrInstr);
    if (VBOX_FAILURE(rc))
    {
        Log(("EMInterpretDisasOne: Failed to convert %RTsel:%VGv (cpl=%d) - rc=%Vrc !!\n",
             pCtxCore->cs, pCtxCore->rip, pCtxCore->ss & X86_SEL_RPL, rc));
        return rc;
    }
    return EMInterpretDisasOneEx(pVM, (RTGCUINTPTR)GCPtrInstr, pCtxCore, pCpu, pcbInstr);
}


/**
 * Disassembles one instruction.
 *
 * This is used by internally by the interpreter and by trap/access handlers.
 *
 * @param   pVM             The VM handle.
 * @param   GCPtrInstr      The flat address of the instruction.
 * @param   pCtxCore        The context core (used to determin the cpu mode).
 * @param   pCpu            Where to return the parsed instruction info.
 * @param   pcbInstr        Where to return the instruction size. (optional)
 */
EMDECL(int) EMInterpretDisasOneEx(PVM pVM, RTGCUINTPTR GCPtrInstr, PCCPUMCTXCORE pCtxCore, PDISCPUSTATE pCpu, unsigned *pcbInstr)
{
    int rc = DISCoreOneEx(GCPtrInstr, SELMGetCpuModeFromSelector(pVM, pCtxCore->eflags, pCtxCore->cs, (PCPUMSELREGHID)&pCtxCore->csHid),
#ifdef IN_GC
                          NULL, NULL,
#else
                          EMReadBytes, pVM,
#endif
                          pCpu, pcbInstr);
    if (VBOX_SUCCESS(rc))
        return VINF_SUCCESS;
    AssertMsgFailed(("DISCoreOne failed to GCPtrInstr=%VGv rc=%Vrc\n", GCPtrInstr, rc));
    return VERR_INTERNAL_ERROR;
}


/**
 * Interprets the current instruction.
 *
 * @returns VBox status code.
 * @retval  VINF_*                  Scheduling instructions.
 * @retval  VERR_EM_INTERPRETER     Something we can't cope with.
 * @retval  VERR_*                  Fatal errors.
 *
 * @param   pVM         The VM handle.
 * @param   pRegFrame   The register frame.
 *                      Updates the EIP if an instruction was executed successfully.
 * @param   pvFault     The fault address (CR2).
 * @param   pcbSize     Size of the write (if applicable).
 *
 * @remark  Invalid opcode exceptions have a higher priority than GP (see Intel
 *          Architecture System Developers Manual, Vol 3, 5.5) so we don't need
 *          to worry about e.g. invalid modrm combinations (!)
 */
EMDECL(int) EMInterpretInstruction(PVM pVM, PCPUMCTXCORE pRegFrame, RTGCPTR pvFault, uint32_t *pcbSize)
{
    RTGCPTR pbCode;

    LogFlow(("EMInterpretInstruction %VGv fault %VGv\n", pRegFrame->rip, pvFault));
    int rc = SELMValidateAndConvertCSAddr(pVM, pRegFrame->eflags, pRegFrame->ss, pRegFrame->cs, &pRegFrame->csHid, (RTGCPTR)pRegFrame->rip, &pbCode);
    if (VBOX_SUCCESS(rc))
    {
        uint32_t    cbOp;
        DISCPUSTATE Cpu;
        Cpu.mode = SELMGetCpuModeFromSelector(pVM, pRegFrame->eflags, pRegFrame->cs, &pRegFrame->csHid);
        rc = emDisCoreOne(pVM, &Cpu, (RTGCUINTPTR)pbCode, &cbOp);
        if (VBOX_SUCCESS(rc))
        {
            Assert(cbOp == Cpu.opsize);
            rc = EMInterpretInstructionCPU(pVM, &Cpu, pRegFrame, pvFault, pcbSize);
            if (VBOX_SUCCESS(rc))
            {
                pRegFrame->rip += cbOp; /* Move on to the next instruction. */
            }
            return rc;
        }
    }
    return VERR_EM_INTERPRETER;
}

/**
 * Interprets the current instruction using the supplied DISCPUSTATE structure.
 *
 * EIP is *NOT* updated!
 *
 * @returns VBox status code.
 * @retval  VINF_*                  Scheduling instructions. When these are returned, it
 *                                  starts to get a bit tricky to know whether code was
 *                                  executed or not... We'll address this when it becomes a problem.
 * @retval  VERR_EM_INTERPRETER     Something we can't cope with.
 * @retval  VERR_*                  Fatal errors.
 *
 * @param   pVM         The VM handle.
 * @param   pCpu        The disassembler cpu state for the instruction to be interpreted.
 * @param   pRegFrame   The register frame. EIP is *NOT* changed!
 * @param   pvFault     The fault address (CR2).
 * @param   pcbSize     Size of the write (if applicable).
 *
 * @remark  Invalid opcode exceptions have a higher priority than GP (see Intel
 *          Architecture System Developers Manual, Vol 3, 5.5) so we don't need
 *          to worry about e.g. invalid modrm combinations (!)
 *
 * @todo    At this time we do NOT check if the instruction overwrites vital information.
 *          Make sure this can't happen!! (will add some assertions/checks later)
 */
EMDECL(int) EMInterpretInstructionCPU(PVM pVM, PDISCPUSTATE pCpu, PCPUMCTXCORE pRegFrame, RTGCPTR pvFault, uint32_t *pcbSize)
{
    STAM_PROFILE_START(&CTXMID(pVM->em.s.CTXSUFF(pStats)->Stat,Emulate), a);
    int rc = emInterpretInstructionCPU(pVM, pCpu, pRegFrame, pvFault, pcbSize);
    STAM_PROFILE_STOP(&CTXMID(pVM->em.s.CTXSUFF(pStats)->Stat,Emulate), a);
    if (VBOX_SUCCESS(rc))
        STAM_COUNTER_INC(&pVM->em.s.CTXSUFF(pStats)->CTXMID(Stat,InterpretSucceeded));
    else
        STAM_COUNTER_INC(&pVM->em.s.CTXSUFF(pStats)->CTXMID(Stat,InterpretFailed));
    return rc;
}


/**
 * Interpret a port I/O instruction.
 *
 * @returns VBox status code suitable for scheduling.
 * @param   pVM         The VM handle.
 * @param   pCtxCore    The context core. This will be updated on successful return.
 * @param   pCpu        The instruction to interpret.
 * @param   cbOp        The size of the instruction.
 * @remark  This may raise exceptions.
 */
EMDECL(int) EMInterpretPortIO(PVM pVM, PCPUMCTXCORE pCtxCore, PDISCPUSTATE pCpu, uint32_t cbOp)
{
    /*
     * Hand it on to IOM.
     */
#ifdef IN_GC
    int rc = IOMGCIOPortHandler(pVM, pCtxCore, pCpu);
    if (IOM_SUCCESS(rc))
        pCtxCore->rip += cbOp;
    return rc;
#else
    AssertReleaseMsgFailed(("not implemented\n"));
    return VERR_NOT_IMPLEMENTED;
#endif
}


DECLINLINE(int) emRamRead(PVM pVM, void *pDest, RTGCPTR GCSrc, uint32_t cb)
{
#ifdef IN_GC
    int rc = MMGCRamRead(pVM, pDest, (void *)GCSrc, cb);
    if (RT_LIKELY(rc != VERR_ACCESS_DENIED))
        return rc;
    /* 
     * The page pool cache may end up here in some cases because it 
     * flushed one of the shadow mappings used by the trapping 
     * instruction and it either flushed the TLB or the CPU reused it.
     */
    RTGCPHYS GCPhys;
    rc = PGMPhysGCPtr2GCPhys(pVM, GCSrc, &GCPhys);
    AssertRCReturn(rc, rc);
    PGMPhysRead(pVM, GCPhys, pDest, cb);
    return VINF_SUCCESS;
#else
    return PGMPhysReadGCPtrSafe(pVM, pDest, GCSrc, cb);
#endif
}

DECLINLINE(int) emRamWrite(PVM pVM, RTGCPTR GCDest, void *pSrc, uint32_t cb)
{
#ifdef IN_GC
    int rc = MMGCRamWrite(pVM, (void *)GCDest, pSrc, cb);
    if (RT_LIKELY(rc != VERR_ACCESS_DENIED))
        return rc;
    /* 
     * The page pool cache may end up here in some cases because it 
     * flushed one of the shadow mappings used by the trapping 
     * instruction and it either flushed the TLB or the CPU reused it.
     * We want to play safe here, verifying that we've got write 
     * access doesn't cost us much (see PGMPhysGCPtr2GCPhys()).
     */
    uint64_t fFlags;
    RTGCPHYS GCPhys;
    rc = PGMGstGetPage(pVM, GCDest, &fFlags, &GCPhys);
    if (RT_FAILURE(rc))
        return rc;
    if (    !(fFlags & X86_PTE_RW) 
        &&  (CPUMGetGuestCR0(pVM) & X86_CR0_WP))
        return VERR_ACCESS_DENIED;

    PGMPhysWrite(pVM, GCPhys + ((RTGCUINTPTR)GCDest & PAGE_OFFSET_MASK), pSrc, cb);
    return VINF_SUCCESS;

#else
    return PGMPhysWriteGCPtrSafe(pVM, GCDest, pSrc, cb);
#endif
}

/* Convert sel:addr to a flat GC address */
static RTGCPTR emConvertToFlatAddr(PVM pVM, PCPUMCTXCORE pRegFrame, PDISCPUSTATE pCpu, POP_PARAMETER pParam, RTGCPTR pvAddr)
{
    DIS_SELREG enmPrefixSeg = DISDetectSegReg(pCpu, pParam);
    return SELMToFlat(pVM, enmPrefixSeg, pRegFrame, pvAddr);
}

#if defined(VBOX_STRICT) || defined(LOG_ENABLED)
/** 
 * Get the mnemonic for the disassembled instruction.
 *  
 * GC/R0 doesn't include the strings in the DIS tables because 
 * of limited space. 
 */ 
static const char *emGetMnemonic(PDISCPUSTATE pCpu)
{
    switch (pCpu->pCurInstr->opcode)
    {
        case OP_XCHG:       return "Xchg";
        case OP_DEC:        return "Dec";
        case OP_INC:        return "Inc";
        case OP_POP:        return "Pop";
        case OP_OR:         return "Or";
        case OP_AND:        return "And";
        case OP_MOV:        return "Mov";
        case OP_INVLPG:     return "InvlPg";
        case OP_CPUID:      return "CpuId";
        case OP_MOV_CR:     return "MovCRx";
        case OP_MOV_DR:     return "MovDRx";
        case OP_LLDT:       return "LLdt";
        case OP_CLTS:       return "Clts";
        case OP_MONITOR:    return "Monitor";
        case OP_MWAIT:      return "MWait";
        case OP_RDMSR:      return "Rdmsr";
        case OP_WRMSR:      return "Wrmsr";
        case OP_ADC:        return "Adc";
        case OP_BTC:        return "Btc";
        case OP_RDTSC:      return "Rdtsc";
        case OP_STI:        return "Sti";
        case OP_XADD:       return "XAdd";
        case OP_HLT:        return "Hlt";
        case OP_IRET:       return "Iret";
        case OP_CMPXCHG:    return "CmpXchg";
        case OP_CMPXCHG8B:  return "CmpXchg8b";
        case OP_MOVNTPS:    return "MovNTPS";
        case OP_STOSWD:     return "StosWD";
        case OP_WBINVD:     return "WbInvd";
        case OP_XOR:        return "Xor";
        case OP_BTR:        return "Btr";
        case OP_BTS:        return "Bts";
        default:
            Log(("Unknown opcode %d\n", pCpu->pCurInstr->opcode));
            return "???";
    }
}
#endif

/**
 * XCHG instruction emulation.
 */
static int emInterpretXchg(PVM pVM, PDISCPUSTATE pCpu, PCPUMCTXCORE pRegFrame, RTGCPTR pvFault, uint32_t *pcbSize)
{
    OP_PARAMVAL param1, param2;

    /* Source to make DISQueryParamVal read the register value - ugly hack */
    int rc = DISQueryParamVal(pRegFrame, pCpu, &pCpu->param1, &param1, PARAM_SOURCE);
    if(VBOX_FAILURE(rc))
        return VERR_EM_INTERPRETER;

    rc = DISQueryParamVal(pRegFrame, pCpu, &pCpu->param2, &param2, PARAM_SOURCE);
    if(VBOX_FAILURE(rc))
        return VERR_EM_INTERPRETER;

#ifdef IN_GC
    if (TRPMHasTrap(pVM))
    {
        if (TRPMGetErrorCode(pVM) & X86_TRAP_PF_RW)
        {
#endif
            RTGCPTR pParam1 = 0, pParam2 = 0;
            uint64_t valpar1, valpar2;

            AssertReturn(pCpu->param1.size == pCpu->param2.size, VERR_EM_INTERPRETER);
            switch(param1.type)
            {
            case PARMTYPE_IMMEDIATE: /* register type is translated to this one too */
                valpar1 = param1.val.val64;
                break;

            case PARMTYPE_ADDRESS:
                pParam1 = (RTGCPTR)param1.val.val64;
                pParam1 = emConvertToFlatAddr(pVM, pRegFrame, pCpu, &pCpu->param1, pParam1);
#ifdef IN_GC
                /* Safety check (in theory it could cross a page boundary and fault there though) */
                AssertReturn(pParam1 == pvFault, VERR_EM_INTERPRETER);
#endif
                rc = emRamRead(pVM, &valpar1, pParam1, param1.size);
                if (VBOX_FAILURE(rc))
                {
                    AssertMsgFailed(("MMGCRamRead %VGv size=%d failed with %Vrc\n", pParam1, param1.size, rc));
                    return VERR_EM_INTERPRETER;
                }
                break;

            default:
                AssertFailed();
                return VERR_EM_INTERPRETER;
            }

            switch(param2.type)
            {
            case PARMTYPE_ADDRESS:
                pParam2 = (RTGCPTR)param2.val.val64;
                pParam2 = emConvertToFlatAddr(pVM, pRegFrame, pCpu, &pCpu->param2, pParam2);
#ifdef IN_GC
                /* Safety check (in theory it could cross a page boundary and fault there though) */
                AssertReturn(pParam2 == pvFault, VERR_EM_INTERPRETER);
#endif
                rc = emRamRead(pVM,  &valpar2, pParam2, param2.size);
                if (VBOX_FAILURE(rc))
                {
                    AssertMsgFailed(("MMGCRamRead %VGv size=%d failed with %Vrc\n", pParam1, param1.size, rc));
                }
                break;

            case PARMTYPE_IMMEDIATE:
                valpar2 = param2.val.val64;
                break;

            default:
                AssertFailed();
                return VERR_EM_INTERPRETER;
            }

            /* Write value of parameter 2 to parameter 1 (reg or memory address) */
            if (pParam1 == 0)
            {
                Assert(param1.type == PARMTYPE_IMMEDIATE); /* register actually */
                switch(param1.size)
                {
                case 1: //special case for AH etc
                        rc = DISWriteReg8(pRegFrame, pCpu->param1.base.reg_gen,  (uint8_t )valpar2); break;
                case 2: rc = DISWriteReg16(pRegFrame, pCpu->param1.base.reg_gen, (uint16_t)valpar2); break;
                case 4: rc = DISWriteReg32(pRegFrame, pCpu->param1.base.reg_gen, (uint32_t)valpar2); break;
                case 8: rc = DISWriteReg64(pRegFrame, pCpu->param1.base.reg_gen, valpar2); break;
                default: AssertFailedReturn(VERR_EM_INTERPRETER);
                }
                if (VBOX_FAILURE(rc))
                    return VERR_EM_INTERPRETER;
            }
            else
            {
                rc = emRamWrite(pVM, pParam1, &valpar2, param1.size);
                if (VBOX_FAILURE(rc))
                {
                    AssertMsgFailed(("emRamWrite %VGv size=%d failed with %Vrc\n", pParam1, param1.size, rc));
                    return VERR_EM_INTERPRETER;
                }
            }

            /* Write value of parameter 1 to parameter 2 (reg or memory address) */
            if (pParam2 == 0)
            {
                Assert(param2.type == PARMTYPE_IMMEDIATE); /* register actually */
                switch(param2.size)
                {
                case 1: //special case for AH etc
                        rc = DISWriteReg8(pRegFrame, pCpu->param2.base.reg_gen,  (uint8_t )valpar1);    break;
                case 2: rc = DISWriteReg16(pRegFrame, pCpu->param2.base.reg_gen, (uint16_t)valpar1);    break;
                case 4: rc = DISWriteReg32(pRegFrame, pCpu->param2.base.reg_gen, (uint32_t)valpar1);    break;
                case 8: rc = DISWriteReg64(pRegFrame, pCpu->param2.base.reg_gen, valpar1);              break;
                default: AssertFailedReturn(VERR_EM_INTERPRETER);
                }
                if (VBOX_FAILURE(rc))
                    return VERR_EM_INTERPRETER;
            }
            else
            {
                rc = emRamWrite(pVM, pParam2, &valpar1, param2.size);
                if (VBOX_FAILURE(rc))
                {
                    AssertMsgFailed(("emRamWrite %VGv size=%d failed with %Vrc\n", pParam1, param1.size, rc));
                    return VERR_EM_INTERPRETER;
                }
            }

            *pcbSize = param2.size;
            return VINF_SUCCESS;
#ifdef IN_GC
        }
    }
#endif
    return VERR_EM_INTERPRETER;
}

/**
 * INC and DEC emulation.
 */
static int emInterpretIncDec(PVM pVM, PDISCPUSTATE pCpu, PCPUMCTXCORE pRegFrame, RTGCPTR pvFault, uint32_t *pcbSize,
                             PFN_EMULATE_PARAM2 pfnEmulate)
{
    OP_PARAMVAL param1;

    int rc = DISQueryParamVal(pRegFrame, pCpu, &pCpu->param1, &param1, PARAM_DEST);
    if(VBOX_FAILURE(rc))
        return VERR_EM_INTERPRETER;

#ifdef IN_GC
    if (TRPMHasTrap(pVM))
    {
        if (TRPMGetErrorCode(pVM) & X86_TRAP_PF_RW)
        {
#endif
            RTGCPTR pParam1 = 0;
            uint64_t valpar1;

            if (param1.type == PARMTYPE_ADDRESS)
            {
                pParam1 = (RTGCPTR)param1.val.val64;
                pParam1 = emConvertToFlatAddr(pVM, pRegFrame, pCpu, &pCpu->param1, pParam1);
#ifdef IN_GC
                /* Safety check (in theory it could cross a page boundary and fault there though) */
                AssertReturn(pParam1 == pvFault, VERR_EM_INTERPRETER);
#endif
                rc = emRamRead(pVM,  &valpar1, pParam1, param1.size);
                if (VBOX_FAILURE(rc))
                {
                    AssertMsgFailed(("emRamRead %VGv size=%d failed with %Vrc\n", pParam1, param1.size, rc));
                    return VERR_EM_INTERPRETER;
                }
            }
            else
            {
                AssertFailed();
                return VERR_EM_INTERPRETER;
            }

            uint32_t eflags;

            eflags = pfnEmulate(&valpar1, param1.size);

            /* Write result back */
            rc = emRamWrite(pVM, pParam1, &valpar1, param1.size);
            if (VBOX_FAILURE(rc))
            {
                AssertMsgFailed(("emRamWrite %VGv size=%d failed with %Vrc\n", pParam1, param1.size, rc));
                return VERR_EM_INTERPRETER;
            }

            /* Update guest's eflags and finish. */
            pRegFrame->eflags.u32 = (pRegFrame->eflags.u32   & ~(X86_EFL_PF | X86_EFL_AF | X86_EFL_ZF | X86_EFL_SF | X86_EFL_OF))
                                  | (eflags & (X86_EFL_PF | X86_EFL_AF | X86_EFL_ZF | X86_EFL_SF | X86_EFL_OF));

            /* All done! */
            *pcbSize = param1.size;
            return VINF_SUCCESS;
#ifdef IN_GC
        }
    }
#endif
    return VERR_EM_INTERPRETER;
}

/**
 * POP Emulation.
 */
static int emInterpretPop(PVM pVM, PDISCPUSTATE pCpu, PCPUMCTXCORE pRegFrame, RTGCPTR pvFault, uint32_t *pcbSize)
{
    Assert(pCpu->mode != CPUMODE_64BIT);    /** @todo check */
    OP_PARAMVAL param1;
    int rc = DISQueryParamVal(pRegFrame, pCpu, &pCpu->param1, &param1, PARAM_DEST);
    if(VBOX_FAILURE(rc))
        return VERR_EM_INTERPRETER;

#ifdef IN_GC
    if (TRPMHasTrap(pVM))
    {
        if (TRPMGetErrorCode(pVM) & X86_TRAP_PF_RW)
        {
#endif
            RTGCPTR pParam1 = 0;
            uint32_t valpar1;
            RTGCPTR pStackVal;

            /* Read stack value first */
            if (SELMGetCpuModeFromSelector(pVM, pRegFrame->eflags, pRegFrame->ss, &pRegFrame->ssHid) == CPUMODE_16BIT)
                return VERR_EM_INTERPRETER; /* No legacy 16 bits stuff here, please. */

            /* Convert address; don't bother checking limits etc, as we only read here */
            pStackVal = SELMToFlat(pVM, DIS_SELREG_SS, pRegFrame, (RTGCPTR)pRegFrame->esp);
            if (pStackVal == 0)
                return VERR_EM_INTERPRETER;

            rc = emRamRead(pVM,  &valpar1, pStackVal, param1.size);
            if (VBOX_FAILURE(rc))
            {
                AssertMsgFailed(("emRamRead %VGv size=%d failed with %Vrc\n", pParam1, param1.size, rc));
                return VERR_EM_INTERPRETER;
            }

            if (param1.type == PARMTYPE_ADDRESS)
            {
                pParam1 = (RTGCPTR)param1.val.val64;

                /* pop [esp+xx] uses esp after the actual pop! */
                AssertCompile(USE_REG_ESP == USE_REG_SP);
                if (    (pCpu->param1.flags & USE_BASE)
                    &&  (pCpu->param1.flags & (USE_REG_GEN16|USE_REG_GEN32))
                    &&  pCpu->param1.base.reg_gen == USE_REG_ESP
                   )
                   pParam1 = (RTGCPTR)((RTGCUINTPTR)pParam1 + param1.size);

                pParam1 = emConvertToFlatAddr(pVM, pRegFrame, pCpu, &pCpu->param1, pParam1);

#ifdef IN_GC
                /* Safety check (in theory it could cross a page boundary and fault there though) */
                AssertMsgReturn(pParam1 == pvFault || (RTGCPTR)pRegFrame->esp == pvFault, ("%VGv != %VGv ss:esp=%04X:%08x\n", pParam1, pvFault, pRegFrame->ss, pRegFrame->esp), VERR_EM_INTERPRETER);
#endif
                rc = emRamWrite(pVM, pParam1, &valpar1, param1.size);
                if (VBOX_FAILURE(rc))
                {
                    AssertMsgFailed(("emRamWrite %VGv size=%d failed with %Vrc\n", pParam1, param1.size, rc));
                    return VERR_EM_INTERPRETER;
                }

                /* Update ESP as the last step */
                pRegFrame->esp += param1.size;
            }
            else
            {
#ifndef DEBUG_bird // annoying assertion.
                AssertFailed();
#endif
                return VERR_EM_INTERPRETER;
            }

            /* All done! */
            *pcbSize = param1.size;
            return VINF_SUCCESS;
#ifdef IN_GC
        }
    }
#endif
    return VERR_EM_INTERPRETER;
}


/**
 * XOR/OR/AND Emulation.
 */
static int emInterpretOrXorAnd(PVM pVM, PDISCPUSTATE pCpu, PCPUMCTXCORE pRegFrame, RTGCPTR pvFault, uint32_t *pcbSize,
                               PFN_EMULATE_PARAM3 pfnEmulate)
{
    OP_PARAMVAL param1, param2;
    int rc = DISQueryParamVal(pRegFrame, pCpu, &pCpu->param1, &param1, PARAM_DEST);
    if(VBOX_FAILURE(rc))
        return VERR_EM_INTERPRETER;

    rc = DISQueryParamVal(pRegFrame, pCpu, &pCpu->param2, &param2, PARAM_SOURCE);
    if(VBOX_FAILURE(rc))
        return VERR_EM_INTERPRETER;

#ifdef LOG_ENABLED
    const char *pszInstr;

    if (pCpu->pCurInstr->opcode == OP_XOR)
        pszInstr = "Xor";
    else if (pCpu->pCurInstr->opcode == OP_OR)
        pszInstr = "Or";
    else if (pCpu->pCurInstr->opcode == OP_AND)
        pszInstr = "And";
    else
        pszInstr = "OrXorAnd??";
#endif

#ifdef IN_GC
    if (TRPMHasTrap(pVM))
    {
        if (TRPMGetErrorCode(pVM) & X86_TRAP_PF_RW)
        {
#endif
            RTGCPTR  pParam1;
            uint64_t valpar1, valpar2;

            if (pCpu->param1.size != pCpu->param2.size)
            {
                if (pCpu->param1.size < pCpu->param2.size)
                {
                    AssertMsgFailed(("%s at %VGv parameter mismatch %d vs %d!!\n", pszInstr, pRegFrame->rip, pCpu->param1.size, pCpu->param2.size)); /* should never happen! */
                    return VERR_EM_INTERPRETER;
                }
                /* Or %Ev, Ib -> just a hack to save some space; the data width of the 1st parameter determines the real width */
                pCpu->param2.size = pCpu->param1.size;
                param2.size     = param1.size;
            }

            /* The destination is always a virtual address */
            if (param1.type == PARMTYPE_ADDRESS)
            {
                pParam1 = (RTGCPTR)param1.val.val64;
                pParam1 = emConvertToFlatAddr(pVM, pRegFrame, pCpu, &pCpu->param1, pParam1);

#ifdef IN_GC
                /* Safety check (in theory it could cross a page boundary and fault there though) */
                AssertMsgReturn(pParam1 == pvFault, ("eip=%VGv, pParam1=%VGv pvFault=%VGv\n", pRegFrame->rip, pParam1, pvFault), VERR_EM_INTERPRETER);
#endif
                rc = emRamRead(pVM,  &valpar1, pParam1, param1.size);
                if (VBOX_FAILURE(rc))
                {
                    AssertMsgFailed(("emRamRead %VGv size=%d failed with %Vrc\n", pParam1, param1.size, rc));
                    return VERR_EM_INTERPRETER;
                }
            }
            else
            {
                AssertFailed();
                return VERR_EM_INTERPRETER;
            }

            /* Register or immediate data */
            switch(param2.type)
            {
            case PARMTYPE_IMMEDIATE:    /* both immediate data and register (ugly) */
                valpar2 = param2.val.val64;
                break;

            default:
                AssertFailed();
                return VERR_EM_INTERPRETER;
            }

            /* Data read, emulate instruction. */
            uint32_t eflags = pfnEmulate(&valpar1, valpar2, param2.size);

            /* Update guest's eflags and finish. */
            pRegFrame->eflags.u32 = (pRegFrame->eflags.u32 & ~(X86_EFL_CF | X86_EFL_PF | X86_EFL_AF | X86_EFL_ZF | X86_EFL_SF | X86_EFL_OF))
                                  | (eflags                &  (X86_EFL_CF | X86_EFL_PF | X86_EFL_AF | X86_EFL_ZF | X86_EFL_SF | X86_EFL_OF));

            /* And write it back */
            rc = emRamWrite(pVM, pParam1, &valpar1, param1.size);
            if (VBOX_SUCCESS(rc))
            {
                /* All done! */
                *pcbSize = param2.size;
                return VINF_SUCCESS;
            }
#ifdef IN_GC
        }
    }
#endif
    return VERR_EM_INTERPRETER;
}

/**
 * LOCK XOR/OR/AND Emulation.
 */
static int emInterpretLockOrXorAnd(PVM pVM, PDISCPUSTATE pCpu, PCPUMCTXCORE pRegFrame, RTGCPTR pvFault, 
                                   uint32_t *pcbSize, PFNEMULATELOCKPARAM3 pfnEmulate)
{
    void *pvParam1;

    OP_PARAMVAL param1, param2;
    int rc = DISQueryParamVal(pRegFrame, pCpu, &pCpu->param1, &param1, PARAM_DEST);
    if(VBOX_FAILURE(rc))
        return VERR_EM_INTERPRETER;

    rc = DISQueryParamVal(pRegFrame, pCpu, &pCpu->param2, &param2, PARAM_SOURCE);
    if(VBOX_FAILURE(rc))
        return VERR_EM_INTERPRETER;

    if (pCpu->param1.size != pCpu->param2.size)
    {
        AssertMsgReturn(pCpu->param1.size >= pCpu->param2.size, /* should never happen! */
                        ("%s at %VGv parameter mismatch %d vs %d!!\n", emGetMnemonic(pCpu), pRegFrame->rip, pCpu->param1.size, pCpu->param2.size),
                        VERR_EM_INTERPRETER);

        /* Or %Ev, Ib -> just a hack to save some space; the data width of the 1st parameter determines the real width */
        pCpu->param2.size = pCpu->param1.size;
        param2.size       = param1.size;
    }

    /* The destination is always a virtual address */
    AssertReturn(param1.type == PARMTYPE_ADDRESS, VERR_EM_INTERPRETER);

    RTGCPTR GCPtrPar1 = param1.val.val64;
    GCPtrPar1 = emConvertToFlatAddr(pVM, pRegFrame, pCpu, &pCpu->param1, GCPtrPar1);
#ifdef IN_GC
    pvParam1  = (void *)GCPtrPar1;
#else
    rc = PGMPhysGCPtr2HCPtr(pVM, GCPtrPar1, &pvParam1);
    if (VBOX_FAILURE(rc))
    {
        AssertRC(rc);
        return VERR_EM_INTERPRETER;
    }
#endif

# ifdef IN_GC
    /* Safety check (in theory it could cross a page boundary and fault there though) */
    Assert(   TRPMHasTrap(pVM)
           && (TRPMGetErrorCode(pVM) & X86_TRAP_PF_RW));
    AssertMsgReturn(GCPtrPar1 == pvFault, ("eip=%VGv, GCPtrPar1=%VGv pvFault=%VGv\n", pRegFrame->rip, GCPtrPar1, pvFault), VERR_EM_INTERPRETER);
# endif

    /* Register and immediate data == PARMTYPE_IMMEDIATE */
    AssertReturn(param2.type == PARMTYPE_IMMEDIATE, VERR_EM_INTERPRETER);
    RTGCUINTREG ValPar2 = param2.val.val64;

    /* Try emulate it with a one-shot #PF handler in place. */
    Log2(("%s %VGv imm%d=%RX64\n", emGetMnemonic(pCpu), GCPtrPar1, pCpu->param2.size*8, ValPar2));

    RTGCUINTREG32 eflags = 0;
#ifdef IN_GC
    MMGCRamRegisterTrapHandler(pVM);
#endif
    rc = pfnEmulate(pvParam1, ValPar2, pCpu->param2.size, &eflags);
#ifdef IN_GC
    MMGCRamDeregisterTrapHandler(pVM);
#endif
    if (RT_FAILURE(rc))
    {
        Log(("%s %VGv imm%d=%RX64-> emulation failed due to page fault!\n", emGetMnemonic(pCpu), GCPtrPar1, pCpu->param2.size*8, ValPar2));
        return VERR_EM_INTERPRETER;
    }

    /* Update guest's eflags and finish. */
    pRegFrame->eflags.u32 = (pRegFrame->eflags.u32 & ~(X86_EFL_CF | X86_EFL_PF | X86_EFL_AF | X86_EFL_ZF | X86_EFL_SF | X86_EFL_OF))
                          | (eflags                &  (X86_EFL_CF | X86_EFL_PF | X86_EFL_AF | X86_EFL_ZF | X86_EFL_SF | X86_EFL_OF));

    *pcbSize = param2.size;
    return VINF_SUCCESS;
}

/**
 * ADD, ADC & SUB Emulation.
 */
static int emInterpretAddSub(PVM pVM, PDISCPUSTATE pCpu, PCPUMCTXCORE pRegFrame, RTGCPTR pvFault, uint32_t *pcbSize,
                             PFN_EMULATE_PARAM3 pfnEmulate)
{
    OP_PARAMVAL param1, param2;
    int rc = DISQueryParamVal(pRegFrame, pCpu, &pCpu->param1, &param1, PARAM_DEST);
    if(VBOX_FAILURE(rc))
        return VERR_EM_INTERPRETER;

    rc = DISQueryParamVal(pRegFrame, pCpu, &pCpu->param2, &param2, PARAM_SOURCE);
    if(VBOX_FAILURE(rc))
        return VERR_EM_INTERPRETER;

#ifdef LOG_ENABLED
    const char *pszInstr;

    if (pCpu->pCurInstr->opcode == OP_SUB)
        pszInstr = "Sub";
    else if (pCpu->pCurInstr->opcode == OP_ADD)
        pszInstr = "Add";
    else if (pCpu->pCurInstr->opcode == OP_ADC)
        pszInstr = "Adc";
    else
        pszInstr = "AddSub??";
#endif

#ifdef IN_GC
    if (TRPMHasTrap(pVM))
    {
        if (TRPMGetErrorCode(pVM) & X86_TRAP_PF_RW)
        {
#endif
            RTGCPTR  pParam1;
            uint64_t valpar1, valpar2;

            if (pCpu->param1.size != pCpu->param2.size)
            {
                if (pCpu->param1.size < pCpu->param2.size)
                {
                    AssertMsgFailed(("%s at %VGv parameter mismatch %d vs %d!!\n", pszInstr, pRegFrame->rip, pCpu->param1.size, pCpu->param2.size)); /* should never happen! */
                    return VERR_EM_INTERPRETER;
                }
                /* Or %Ev, Ib -> just a hack to save some space; the data width of the 1st parameter determines the real width */
                pCpu->param2.size = pCpu->param1.size;
                param2.size     = param1.size;
            }

            /* The destination is always a virtual address */
            if (param1.type == PARMTYPE_ADDRESS)
            {
                pParam1 = (RTGCPTR)param1.val.val64;
                pParam1 = emConvertToFlatAddr(pVM, pRegFrame, pCpu, &pCpu->param1, pParam1);

#ifdef IN_GC
                /* Safety check (in theory it could cross a page boundary and fault there though) */
                AssertReturn(pParam1 == pvFault, VERR_EM_INTERPRETER);
#endif
                rc = emRamRead(pVM,  &valpar1, pParam1, param1.size);
                if (VBOX_FAILURE(rc))
                {
                    AssertMsgFailed(("emRamRead %VGv size=%d failed with %Vrc\n", pParam1, param1.size, rc));
                    return VERR_EM_INTERPRETER;
                }
            }
            else
            {
#ifndef DEBUG_bird
                AssertFailed();
#endif
                return VERR_EM_INTERPRETER;
            }

            /* Register or immediate data */
            switch(param2.type)
            {
            case PARMTYPE_IMMEDIATE:    /* both immediate data and register (ugly) */
                valpar2 = param2.val.val64;
                break;

            default:
                AssertFailed();
                return VERR_EM_INTERPRETER;
            }

            /* Data read, emulate instruction. */
            uint32_t eflags = pfnEmulate(&valpar1, valpar2, param2.size);

            /* Update guest's eflags and finish. */
            pRegFrame->eflags.u32 = (pRegFrame->eflags.u32 & ~(X86_EFL_CF | X86_EFL_PF | X86_EFL_AF | X86_EFL_ZF | X86_EFL_SF | X86_EFL_OF))
                                  | (eflags                &  (X86_EFL_CF | X86_EFL_PF | X86_EFL_AF | X86_EFL_ZF | X86_EFL_SF | X86_EFL_OF));

            /* And write it back */
            rc = emRamWrite(pVM, pParam1, &valpar1, param1.size);
            if (VBOX_SUCCESS(rc))
            {
                /* All done! */
                *pcbSize = param2.size;
                return VINF_SUCCESS;
            }
#ifdef IN_GC
        }
    }
#endif
    return VERR_EM_INTERPRETER;
}

/**
 * ADC Emulation.
 */
static int emInterpretAdc(PVM pVM, PDISCPUSTATE pCpu, PCPUMCTXCORE pRegFrame, RTGCPTR pvFault, uint32_t *pcbSize)
{
    if (pRegFrame->eflags.Bits.u1CF)
        return emInterpretAddSub(pVM, pCpu, pRegFrame, pvFault, pcbSize, EMEmulateAdcWithCarrySet);
    else
        return emInterpretAddSub(pVM, pCpu, pRegFrame, pvFault, pcbSize, EMEmulateAdd);
}

/**
 * BTR/C/S Emulation.
 */
static int emInterpretBitTest(PVM pVM, PDISCPUSTATE pCpu, PCPUMCTXCORE pRegFrame, RTGCPTR pvFault, uint32_t *pcbSize,
                              PFN_EMULATE_PARAM2_UINT32 pfnEmulate)
{
    OP_PARAMVAL param1, param2;
    int rc = DISQueryParamVal(pRegFrame, pCpu, &pCpu->param1, &param1, PARAM_DEST);
    if(VBOX_FAILURE(rc))
        return VERR_EM_INTERPRETER;

    rc = DISQueryParamVal(pRegFrame, pCpu, &pCpu->param2, &param2, PARAM_SOURCE);
    if(VBOX_FAILURE(rc))
        return VERR_EM_INTERPRETER;

#ifdef LOG_ENABLED
    const char *pszInstr;

    if (pCpu->pCurInstr->opcode == OP_BTR)
        pszInstr = "Btr";
    else if (pCpu->pCurInstr->opcode == OP_BTS)
        pszInstr = "Bts";
    else if (pCpu->pCurInstr->opcode == OP_BTC)
        pszInstr = "Btc";
    else
        pszInstr = "Bit??";
#endif

#ifdef IN_GC
    if (TRPMHasTrap(pVM))
    {
        if (TRPMGetErrorCode(pVM) & X86_TRAP_PF_RW)
        {
#endif
            RTGCPTR  pParam1;
            uint64_t valpar1 = 0, valpar2;
            uint32_t eflags;

            /* The destination is always a virtual address */
            if (param1.type != PARMTYPE_ADDRESS)
                return VERR_EM_INTERPRETER;

            pParam1 = (RTGCPTR)param1.val.val64;
            pParam1 = emConvertToFlatAddr(pVM, pRegFrame, pCpu, &pCpu->param1, pParam1);

            /* Register or immediate data */
            switch(param2.type)
            {
            case PARMTYPE_IMMEDIATE:    /* both immediate data and register (ugly) */
                valpar2 = param2.val.val64;
                break;

            default:
                AssertFailed();
                return VERR_EM_INTERPRETER;
            }

            Log2(("emInterpret%s: pvFault=%VGv pParam1=%VGv val2=%x\n", pszInstr, pvFault, pParam1, valpar2));
            pParam1 = (RTGCPTR)((RTGCUINTPTR)pParam1 + valpar2/8);
#ifdef IN_GC
            /* Safety check. */
            AssertMsgReturn((RTGCPTR)((RTGCUINTPTR)pParam1 & ~3) == pvFault, ("pParam1=%VGv pvFault=%VGv\n", pParam1, pvFault), VERR_EM_INTERPRETER);
#endif
            rc = emRamRead(pVM, &valpar1, pParam1, 1);
            if (VBOX_FAILURE(rc))
            {
                AssertMsgFailed(("emRamRead %VGv size=%d failed with %Vrc\n", pParam1, param1.size, rc));
                return VERR_EM_INTERPRETER;
            }

            Log2(("emInterpretBtx: val=%x\n", valpar1));
            /* Data read, emulate bit test instruction. */
            eflags = pfnEmulate(&valpar1, valpar2 & 0x7);

            Log2(("emInterpretBtx: val=%x CF=%d\n", valpar1, !!(eflags & X86_EFL_CF)));

            /* Update guest's eflags and finish. */
            pRegFrame->eflags.u32 = (pRegFrame->eflags.u32 & ~(X86_EFL_CF | X86_EFL_PF | X86_EFL_AF | X86_EFL_ZF | X86_EFL_SF | X86_EFL_OF))
                                  | (eflags                &  (X86_EFL_CF | X86_EFL_PF | X86_EFL_AF | X86_EFL_ZF | X86_EFL_SF | X86_EFL_OF));

            /* And write it back */
            rc = emRamWrite(pVM, pParam1, &valpar1, 1);
            if (VBOX_SUCCESS(rc))
            {
                /* All done! */
                *pcbSize = 1;
                return VINF_SUCCESS;
            }
#ifdef IN_GC
        }
    }
#endif
    return VERR_EM_INTERPRETER;
}

/**
 * LOCK BTR/C/S Emulation.
 */
static int emInterpretLockBitTest(PVM pVM, PDISCPUSTATE pCpu, PCPUMCTXCORE pRegFrame, RTGCPTR pvFault, 
                                  uint32_t *pcbSize, PFNEMULATELOCKPARAM2 pfnEmulate)
{
    void *pvParam1;

    OP_PARAMVAL param1, param2;
    int rc = DISQueryParamVal(pRegFrame, pCpu, &pCpu->param1, &param1, PARAM_DEST);
    if(VBOX_FAILURE(rc))
        return VERR_EM_INTERPRETER;

    rc = DISQueryParamVal(pRegFrame, pCpu, &pCpu->param2, &param2, PARAM_SOURCE);
    if(VBOX_FAILURE(rc))
        return VERR_EM_INTERPRETER;

    /* The destination is always a virtual address */
    if (param1.type != PARMTYPE_ADDRESS)
        return VERR_EM_INTERPRETER;

    /* Register and immediate data == PARMTYPE_IMMEDIATE */
    AssertReturn(param2.type == PARMTYPE_IMMEDIATE, VERR_EM_INTERPRETER);
    uint64_t ValPar2 = param2.val.val64;

    /* Adjust the parameters so what we're dealing with is a bit within the byte pointed to. */
    RTGCPTR GCPtrPar1 = param1.val.val64;
    GCPtrPar1 = (GCPtrPar1 + ValPar2 / 8);
    ValPar2 &= 7;

#ifdef IN_GC
    GCPtrPar1 = emConvertToFlatAddr(pVM, pRegFrame, pCpu, &pCpu->param1, GCPtrPar1);
    pvParam1  = (void *)GCPtrPar1;
#else
    GCPtrPar1 = emConvertToFlatAddr(pVM, pRegFrame, pCpu, &pCpu->param1, GCPtrPar1);
    rc = PGMPhysGCPtr2HCPtr(pVM, GCPtrPar1, &pvParam1);
    if (VBOX_FAILURE(rc))
    {
        AssertRC(rc);
        return VERR_EM_INTERPRETER;
    }
#endif

    Log2(("emInterpretLockBitTest %s: pvFault=%VGv GCPtrPar1=%VGv imm=%RX64\n", emGetMnemonic(pCpu), pvFault, GCPtrPar1, ValPar2));

#ifdef IN_GC
    Assert(TRPMHasTrap(pVM));
    AssertMsgReturn((RTGCPTR)((RTGCUINTPTR)GCPtrPar1 & ~(RTGCUINTPTR)3) == pvFault, 
                    ("GCPtrPar1=%VGv pvFault=%VGv\n", GCPtrPar1, pvFault), 
                    VERR_EM_INTERPRETER);
#endif

    /* Try emulate it with a one-shot #PF handler in place. */
    RTGCUINTREG32 eflags = 0;
#ifdef IN_GC
    MMGCRamRegisterTrapHandler(pVM);
#endif
    rc = pfnEmulate(pvParam1, ValPar2, &eflags);
#ifdef IN_GC
    MMGCRamDeregisterTrapHandler(pVM);
#endif
    if (RT_FAILURE(rc))
    {
        Log(("emInterpretLockBitTest %s: %VGv imm%d=%RX64 -> emulation failed due to page fault!\n", 
             emGetMnemonic(pCpu), GCPtrPar1, pCpu->param2.size*8, ValPar2));
        return VERR_EM_INTERPRETER;
    }

    Log2(("emInterpretLockBitTest %s: GCPtrPar1=%VGv imm=%VX64 CF=%d\n", emGetMnemonic(pCpu), GCPtrPar1, ValPar2, !!(eflags & X86_EFL_CF)));

    /* Update guest's eflags and finish. */
    pRegFrame->eflags.u32 = (pRegFrame->eflags.u32 & ~(X86_EFL_CF | X86_EFL_PF | X86_EFL_AF | X86_EFL_ZF | X86_EFL_SF | X86_EFL_OF))
                          | (eflags                &  (X86_EFL_CF | X86_EFL_PF | X86_EFL_AF | X86_EFL_ZF | X86_EFL_SF | X86_EFL_OF));

    *pcbSize = 1;
    return VINF_SUCCESS;
}

/**
 * MOV emulation.
 */
static int emInterpretMov(PVM pVM, PDISCPUSTATE pCpu, PCPUMCTXCORE pRegFrame, RTGCPTR pvFault, uint32_t *pcbSize)
{
    OP_PARAMVAL param1, param2;
    int rc = DISQueryParamVal(pRegFrame, pCpu, &pCpu->param1, &param1, PARAM_DEST);
    if(VBOX_FAILURE(rc))
        return VERR_EM_INTERPRETER;

    rc = DISQueryParamVal(pRegFrame, pCpu, &pCpu->param2, &param2, PARAM_SOURCE);
    if(VBOX_FAILURE(rc))
        return VERR_EM_INTERPRETER;

#ifdef IN_GC
    if (TRPMHasTrap(pVM))
    {
        if (TRPMGetErrorCode(pVM) & X86_TRAP_PF_RW)
        {
#else
        /** @todo Make this the default and don't rely on TRPM information. */
        if (param1.type == PARMTYPE_ADDRESS)
        {
#endif
            RTGCPTR pDest;
            uint64_t val64;

            switch(param1.type)
            {
            case PARMTYPE_IMMEDIATE:
                if(!(param1.flags & (PARAM_VAL32|PARAM_VAL64)))
                    return VERR_EM_INTERPRETER;
                /* fallthru */

            case PARMTYPE_ADDRESS:
                pDest = (RTGCPTR)param1.val.val64;
                pDest = emConvertToFlatAddr(pVM, pRegFrame, pCpu, &pCpu->param1, pDest);
                break;

            default:
                AssertFailed();
                return VERR_EM_INTERPRETER;
            }

            switch(param2.type)
            {
            case PARMTYPE_IMMEDIATE: /* register type is translated to this one too */
                val64 = param2.val.val64;
                break;

            default:
                Log(("emInterpretMov: unexpected type=%d eip=%VGv\n", param2.type, pRegFrame->rip));
                return VERR_EM_INTERPRETER;
            }
#ifdef LOG_ENABLED
            if (pCpu->mode == CPUMODE_64BIT)
                LogFlow(("EMInterpretInstruction at %VGv: OP_MOV %VGv <- %RX64 (%d) &val32=%VHv\n", pRegFrame->rip, pDest, val64, param2.size, &val64));
            else
                LogFlow(("EMInterpretInstruction at %VGv: OP_MOV %VGv <- %08X  (%d) &val32=%VHv\n", pRegFrame->rip, pDest, (uint32_t)val64, param2.size, &val64));
#endif

            Assert(param2.size <= 8 && param2.size > 0);

#if 0 /* CSAM/PATM translates aliases which causes this to incorrectly trigger. See #2609 and #1498. */
#ifdef IN_GC
            /* Safety check (in theory it could cross a page boundary and fault there though) */
            AssertMsgReturn(pDest == pvFault, ("eip=%VGv pDest=%VGv pvFault=%VGv\n", pRegFrame->rip, pDest, pvFault), VERR_EM_INTERPRETER);
#endif
#endif
            rc = emRamWrite(pVM, pDest, &val64, param2.size);
            if (VBOX_FAILURE(rc))
                return VERR_EM_INTERPRETER;

            *pcbSize = param2.size;
        }
        else
        { /* read fault */
            RTGCPTR pSrc;
            uint64_t val64;

            /* Source */
            switch(param2.type)
            {
            case PARMTYPE_IMMEDIATE:
                if(!(param2.flags & (PARAM_VAL32|PARAM_VAL64)))
                    return VERR_EM_INTERPRETER;
                /* fallthru */

            case PARMTYPE_ADDRESS:
                pSrc = (RTGCPTR)param2.val.val64;
                pSrc = emConvertToFlatAddr(pVM, pRegFrame, pCpu, &pCpu->param2, pSrc);
                break;

            default:
                return VERR_EM_INTERPRETER;
            }

            Assert(param1.size <= 8 && param1.size > 0);
#ifdef IN_GC
            /* Safety check (in theory it could cross a page boundary and fault there though) */
            AssertReturn(pSrc == pvFault, VERR_EM_INTERPRETER);
#endif
            rc = emRamRead(pVM, &val64, pSrc, param1.size);
            if (VBOX_FAILURE(rc))
                return VERR_EM_INTERPRETER;

            /* Destination */
            switch(param1.type)
            {
            case PARMTYPE_REGISTER:
                switch(param1.size)
                {
                case 1: rc = DISWriteReg8(pRegFrame, pCpu->param1.base.reg_gen,  (uint8_t) val64); break;
                case 2: rc = DISWriteReg16(pRegFrame, pCpu->param1.base.reg_gen, (uint16_t)val64); break;
                case 4: rc = DISWriteReg32(pRegFrame, pCpu->param1.base.reg_gen, (uint32_t)val64); break;
                case 8: rc = DISWriteReg64(pRegFrame, pCpu->param1.base.reg_gen, val64); break;
                default:
                    return VERR_EM_INTERPRETER;
                }
                if (VBOX_FAILURE(rc))
                    return rc;
                break;

            default:
                return VERR_EM_INTERPRETER;
            }
#ifdef LOG_ENABLED
            if (pCpu->mode == CPUMODE_64BIT)
                LogFlow(("EMInterpretInstruction: OP_MOV %VGv -> %RX64 (%d)\n", pSrc, val64, param1.size));
            else
                LogFlow(("EMInterpretInstruction: OP_MOV %VGv -> %08X (%d)\n", pSrc, (uint32_t)val64, param1.size));
#endif
        }
        return VINF_SUCCESS;
#ifdef IN_GC
    }
#endif
    return VERR_EM_INTERPRETER;
}

/*
 * [LOCK] CMPXCHG emulation.
 */
#ifdef IN_GC
static int emInterpretCmpXchg(PVM pVM, PDISCPUSTATE pCpu, PCPUMCTXCORE pRegFrame, RTGCPTR pvFault, uint32_t *pcbSize)
{
    Assert(pCpu->mode != CPUMODE_64BIT);    /** @todo check */
    OP_PARAMVAL param1, param2;

#ifdef LOG_ENABLED
    const char *pszInstr;

    if (pCpu->prefix & PREFIX_LOCK)
        pszInstr = "Lock CmpXchg";
    else
        pszInstr = "CmpXchg";
#endif

    /* Source to make DISQueryParamVal read the register value - ugly hack */
    int rc = DISQueryParamVal(pRegFrame, pCpu, &pCpu->param1, &param1, PARAM_SOURCE);
    if(VBOX_FAILURE(rc))
        return VERR_EM_INTERPRETER;

    rc = DISQueryParamVal(pRegFrame, pCpu, &pCpu->param2, &param2, PARAM_SOURCE);
    if(VBOX_FAILURE(rc))
        return VERR_EM_INTERPRETER;

    if (TRPMHasTrap(pVM))
    {
        if (TRPMGetErrorCode(pVM) & X86_TRAP_PF_RW)
        {
            RTRCPTR pParam1;
            uint32_t valpar, eflags;
#ifdef VBOX_STRICT
            uint32_t valpar1 = 0; /// @todo used uninitialized...
#endif

            AssertReturn(pCpu->param1.size == pCpu->param2.size, VERR_EM_INTERPRETER);
            switch(param1.type)
            {
            case PARMTYPE_ADDRESS:
                pParam1 = (RTRCPTR)param1.val.val64;
                pParam1 = (RTRCPTR)emConvertToFlatAddr(pVM, pRegFrame, pCpu, &pCpu->param1, (RTGCPTR)(RTRCUINTPTR)pParam1);

                /* Safety check (in theory it could cross a page boundary and fault there though) */
                AssertMsgReturn(pParam1 == (RTRCPTR)pvFault, ("eip=%VGv pParam1=%VRv pvFault=%VGv\n", pRegFrame->rip, pParam1, pvFault), VERR_EM_INTERPRETER);
                break;

            default:
                return VERR_EM_INTERPRETER;
            }

            switch(param2.type)
            {
            case PARMTYPE_IMMEDIATE: /* register actually */
                valpar = param2.val.val32;
                break;

            default:
                return VERR_EM_INTERPRETER;
            }

            LogFlow(("%s %VRv=%08x eax=%08x %08x\n", pszInstr, pParam1, valpar1, pRegFrame->eax, valpar));

            MMGCRamRegisterTrapHandler(pVM);
            if (pCpu->prefix & PREFIX_LOCK)
                rc = EMGCEmulateLockCmpXchg(pParam1, &pRegFrame->eax, valpar, pCpu->param2.size, &eflags);
            else
                rc = EMGCEmulateCmpXchg(pParam1, &pRegFrame->eax, valpar, pCpu->param2.size, &eflags);
            MMGCRamDeregisterTrapHandler(pVM);

            if (VBOX_FAILURE(rc))
            {
                Log(("%s %VGv=%08x eax=%08x %08x -> emulation failed due to page fault!\n", pszInstr, pParam1, valpar1, pRegFrame->eax, valpar));
                return VERR_EM_INTERPRETER;
            }

            LogFlow(("%s %VRv=%08x eax=%08x %08x ZF=%d\n", pszInstr, pParam1, valpar1, pRegFrame->eax, valpar, !!(eflags & X86_EFL_ZF)));

            /* Update guest's eflags and finish. */
            pRegFrame->eflags.u32 = (pRegFrame->eflags.u32 & ~(X86_EFL_CF | X86_EFL_PF | X86_EFL_AF | X86_EFL_ZF | X86_EFL_SF | X86_EFL_OF))
                                  | (eflags                &  (X86_EFL_CF | X86_EFL_PF | X86_EFL_AF | X86_EFL_ZF | X86_EFL_SF | X86_EFL_OF));

            *pcbSize = param2.size;
            return VINF_SUCCESS;
        }
    }
    return VERR_EM_INTERPRETER;
}

/*
 * [LOCK] CMPXCHG8B emulation.
 */
static int emInterpretCmpXchg8b(PVM pVM, PDISCPUSTATE pCpu, PCPUMCTXCORE pRegFrame, RTGCPTR pvFault, uint32_t *pcbSize)
{
    Assert(pCpu->mode != CPUMODE_64BIT);    /** @todo check */
    OP_PARAMVAL param1;

#ifdef LOG_ENABLED
    const char *pszInstr;

    if (pCpu->prefix & PREFIX_LOCK)
        pszInstr = "Lock CmpXchg8b";
    else
        pszInstr = "CmpXchg8b";
#endif

    /* Source to make DISQueryParamVal read the register value - ugly hack */
    int rc = DISQueryParamVal(pRegFrame, pCpu, &pCpu->param1, &param1, PARAM_SOURCE);
    if(VBOX_FAILURE(rc))
        return VERR_EM_INTERPRETER;

    if (TRPMHasTrap(pVM))
    {
        if (TRPMGetErrorCode(pVM) & X86_TRAP_PF_RW)
        {
            RTRCPTR pParam1;
            uint32_t eflags;

            AssertReturn(pCpu->param1.size == 8, VERR_EM_INTERPRETER);
            switch(param1.type)
            {
            case PARMTYPE_ADDRESS:
                pParam1 = (RTRCPTR)param1.val.val64;
                pParam1 = (RTRCPTR)emConvertToFlatAddr(pVM, pRegFrame, pCpu, &pCpu->param1, (RTGCPTR)(RTRCUINTPTR)pParam1);

                /* Safety check (in theory it could cross a page boundary and fault there though) */
                AssertMsgReturn(pParam1 == (RTRCPTR)pvFault, ("eip=%VGv pParam1=%VRv pvFault=%VGv\n", pRegFrame->rip, pParam1, pvFault), VERR_EM_INTERPRETER);
                break;

            default:
                return VERR_EM_INTERPRETER;
            }

            LogFlow(("%s %VRv=%08x eax=%08x\n", pszInstr, pParam1, pRegFrame->eax));

            MMGCRamRegisterTrapHandler(pVM);
            if (pCpu->prefix & PREFIX_LOCK)
                rc = EMGCEmulateLockCmpXchg8b(pParam1, &pRegFrame->eax, &pRegFrame->edx, pRegFrame->ebx, pRegFrame->ecx, &eflags);
            else
                rc = EMGCEmulateCmpXchg8b(pParam1, &pRegFrame->eax, &pRegFrame->edx, pRegFrame->ebx, pRegFrame->ecx, &eflags);
            MMGCRamDeregisterTrapHandler(pVM);

            if (VBOX_FAILURE(rc))
            {
                Log(("%s %VGv=%08x eax=%08x -> emulation failed due to page fault!\n", pszInstr, pParam1, pRegFrame->eax));
                return VERR_EM_INTERPRETER;
            }

            LogFlow(("%s %VGv=%08x eax=%08x ZF=%d\n", pszInstr, pParam1, pRegFrame->eax, !!(eflags & X86_EFL_ZF)));

            /* Update guest's eflags and finish; note that *only* ZF is affected. */
            pRegFrame->eflags.u32 = (pRegFrame->eflags.u32 & ~(X86_EFL_ZF))
                                  | (eflags                &  (X86_EFL_ZF));

            *pcbSize = 8;
            return VINF_SUCCESS;
        }
    }
    return VERR_EM_INTERPRETER;
}
#endif

/*
 * [LOCK] XADD emulation.
 */
#ifdef IN_GC
static int emInterpretXAdd(PVM pVM, PDISCPUSTATE pCpu, PCPUMCTXCORE pRegFrame, RTGCPTR pvFault, uint32_t *pcbSize)
{
    Assert(pCpu->mode != CPUMODE_64BIT);    /** @todo check */
    OP_PARAMVAL param1;
    uint32_t *pParamReg2;
    size_t cbSizeParamReg2;

    /* Source to make DISQueryParamVal read the register value - ugly hack */
    int rc = DISQueryParamVal(pRegFrame, pCpu, &pCpu->param1, &param1, PARAM_SOURCE);
    if(VBOX_FAILURE(rc))
        return VERR_EM_INTERPRETER;

    rc = DISQueryParamRegPtr(pRegFrame, pCpu, &pCpu->param2, (void **)&pParamReg2, &cbSizeParamReg2);
    Assert(cbSizeParamReg2 <= 4);
    if(VBOX_FAILURE(rc))
        return VERR_EM_INTERPRETER;

    if (TRPMHasTrap(pVM))
    {
        if (TRPMGetErrorCode(pVM) & X86_TRAP_PF_RW)
        {
            RTRCPTR pParam1;
            uint32_t eflags;
#ifdef VBOX_STRICT
            uint32_t valpar1 = 0; /// @todo used uninitialized...
#endif

            AssertReturn(pCpu->param1.size == pCpu->param2.size, VERR_EM_INTERPRETER);
            switch(param1.type)
            {
            case PARMTYPE_ADDRESS:
                pParam1 = (RTRCPTR)param1.val.val64;
                pParam1 = (RTRCPTR)emConvertToFlatAddr(pVM, pRegFrame, pCpu, &pCpu->param1, (RTGCPTR)(RTRCUINTPTR)pParam1);

                /* Safety check (in theory it could cross a page boundary and fault there though) */
                AssertMsgReturn(pParam1 == (RTRCPTR)pvFault, ("eip=%VGv pParam1=%VRv pvFault=%VGv\n", pRegFrame->rip, pParam1, pvFault), VERR_EM_INTERPRETER);
                break;

            default:
                return VERR_EM_INTERPRETER;
            }

            LogFlow(("XAdd %VRv=%08x reg=%08x\n", pParam1, *pParamReg2));

            MMGCRamRegisterTrapHandler(pVM);
            if (pCpu->prefix & PREFIX_LOCK)
                rc = EMGCEmulateLockXAdd(pParam1, pParamReg2, cbSizeParamReg2, &eflags);
            else
                rc = EMGCEmulateXAdd(pParam1, pParamReg2, cbSizeParamReg2, &eflags);
            MMGCRamDeregisterTrapHandler(pVM);

            if (VBOX_FAILURE(rc))
            {
                Log(("XAdd %VGv=%08x reg=%08x -> emulation failed due to page fault!\n", pParam1, valpar1, *pParamReg2));
                return VERR_EM_INTERPRETER;
            }

            LogFlow(("XAdd %VGv=%08x reg=%08x ZF=%d\n", pParam1, valpar1, *pParamReg2, !!(eflags & X86_EFL_ZF)));

            /* Update guest's eflags and finish. */
            pRegFrame->eflags.u32 = (pRegFrame->eflags.u32 & ~(X86_EFL_CF | X86_EFL_PF | X86_EFL_AF | X86_EFL_ZF | X86_EFL_SF | X86_EFL_OF))
                                  | (eflags                &  (X86_EFL_CF | X86_EFL_PF | X86_EFL_AF | X86_EFL_ZF | X86_EFL_SF | X86_EFL_OF));

            *pcbSize = cbSizeParamReg2;
            return VINF_SUCCESS;
        }
    }
    return VERR_EM_INTERPRETER;
}
#endif

#ifdef IN_GC
/**
 * Interpret IRET (currently only to V86 code)
 *
 * @returns VBox status code.
 * @param   pVM         The VM handle.
 * @param   pRegFrame   The register frame.
 *
 */
EMDECL(int) EMInterpretIret(PVM pVM, PCPUMCTXCORE pRegFrame)
{
    RTGCUINTPTR pIretStack = (RTGCUINTPTR)pRegFrame->esp;
    RTGCUINTPTR eip, cs, esp, ss, eflags, ds, es, fs, gs, uMask;
    int         rc;

    Assert(!CPUMIsGuestIn64BitCode(pVM, pRegFrame));

    rc  = emRamRead(pVM, &eip,      (RTGCPTR)pIretStack      , 4);
    rc |= emRamRead(pVM, &cs,       (RTGCPTR)(pIretStack + 4), 4);
    rc |= emRamRead(pVM, &eflags,   (RTGCPTR)(pIretStack + 8), 4);
    AssertRCReturn(rc, VERR_EM_INTERPRETER);
    AssertReturn(eflags & X86_EFL_VM, VERR_EM_INTERPRETER);

    rc |= emRamRead(pVM, &esp,      (RTGCPTR)(pIretStack + 12), 4);
    rc |= emRamRead(pVM, &ss,       (RTGCPTR)(pIretStack + 16), 4);
    rc |= emRamRead(pVM, &es,       (RTGCPTR)(pIretStack + 20), 4);
    rc |= emRamRead(pVM, &ds,       (RTGCPTR)(pIretStack + 24), 4);
    rc |= emRamRead(pVM, &fs,       (RTGCPTR)(pIretStack + 28), 4);
    rc |= emRamRead(pVM, &gs,       (RTGCPTR)(pIretStack + 32), 4);
    AssertRCReturn(rc, VERR_EM_INTERPRETER);

    pRegFrame->eip = eip & 0xffff;
    pRegFrame->cs  = cs;

    /* Mask away all reserved bits */
    uMask = X86_EFL_CF | X86_EFL_PF | X86_EFL_AF | X86_EFL_ZF | X86_EFL_SF | X86_EFL_TF | X86_EFL_IF | X86_EFL_DF | X86_EFL_OF | X86_EFL_IOPL | X86_EFL_NT | X86_EFL_RF | X86_EFL_VM | X86_EFL_AC | X86_EFL_VIF | X86_EFL_VIP | X86_EFL_ID;
    eflags &= uMask;

#ifndef IN_RING0
    CPUMRawSetEFlags(pVM, pRegFrame, eflags);
#endif
    Assert((pRegFrame->eflags.u32 & (X86_EFL_IF|X86_EFL_IOPL)) == X86_EFL_IF);

    pRegFrame->esp = esp;
    pRegFrame->ss  = ss;
    pRegFrame->ds  = ds;
    pRegFrame->es  = es;
    pRegFrame->fs  = fs;
    pRegFrame->gs  = gs;

    return VINF_SUCCESS;
}
#endif

/**
 * IRET Emulation.
 */
static int emInterpretIret(PVM pVM, PDISCPUSTATE pCpu, PCPUMCTXCORE pRegFrame, RTGCPTR pvFault, uint32_t *pcbSize)
{
    /* only allow direct calls to EMInterpretIret for now */
    return VERR_EM_INTERPRETER;
}

/**
 * INVLPG Emulation.
 */

/**
 * Interpret INVLPG
 *
 * @returns VBox status code.
 * @param   pVM         The VM handle.
 * @param   pRegFrame   The register frame.
 * @param   pAddrGC     Operand address
 *
 */
EMDECL(int) EMInterpretInvlpg(PVM pVM, PCPUMCTXCORE pRegFrame, RTGCPTR pAddrGC)
{
    int rc;

    /** @todo is addr always a flat linear address or ds based
     * (in absence of segment override prefixes)????
     */
#ifdef IN_GC
    // Note: we could also use PGMFlushPage here, but it currently doesn't always use invlpg!!!!!!!!!!
    LogFlow(("GC: EMULATE: invlpg %08X\n", pAddrGC));
    rc = PGMGCInvalidatePage(pVM, pAddrGC);
#else
    rc = PGMInvalidatePage(pVM, pAddrGC);
#endif
    if (VBOX_SUCCESS(rc))
        return VINF_SUCCESS;
    Log(("PGMInvalidatePage %VGv returned %VGv (%d)\n", pAddrGC, rc, rc));
    Assert(rc == VERR_REM_FLUSHED_PAGES_OVERFLOW);
    /** @todo r=bird: we shouldn't ignore returns codes like this... I'm 99% sure the error is fatal. */
    return VERR_EM_INTERPRETER;
}

static int emInterpretInvlPg(PVM pVM, PDISCPUSTATE pCpu, PCPUMCTXCORE pRegFrame, RTGCPTR pvFault, uint32_t *pcbSize)
{
    OP_PARAMVAL param1;
    RTGCPTR     addr;

    int rc = DISQueryParamVal(pRegFrame, pCpu, &pCpu->param1, &param1, PARAM_SOURCE);
    if(VBOX_FAILURE(rc))
        return VERR_EM_INTERPRETER;

    switch(param1.type)
    {
    case PARMTYPE_IMMEDIATE:
    case PARMTYPE_ADDRESS:
        if(!(param1.flags & (PARAM_VAL32|PARAM_VAL64)))
            return VERR_EM_INTERPRETER;
        addr = (RTGCPTR)param1.val.val64;
        break;

    default:
        return VERR_EM_INTERPRETER;
    }

    /** @todo is addr always a flat linear address or ds based
     * (in absence of segment override prefixes)????
     */
#ifdef IN_GC
    // Note: we could also use PGMFlushPage here, but it currently doesn't always use invlpg!!!!!!!!!!
    LogFlow(("GC: EMULATE: invlpg %08X\n", addr));
    rc = PGMGCInvalidatePage(pVM, addr);
#else
    rc = PGMInvalidatePage(pVM, addr);
#endif
    if (VBOX_SUCCESS(rc))
        return VINF_SUCCESS;
    /** @todo r=bird: we shouldn't ignore returns codes like this... I'm 99% sure the error is fatal. */
    return VERR_EM_INTERPRETER;
}

/**
 * CPUID Emulation.
 */

/**
 * Interpret CPUID given the parameters in the CPU context
 *
 * @returns VBox status code.
 * @param   pVM         The VM handle.
 * @param   pRegFrame   The register frame.
 *
 */
EMDECL(int) EMInterpretCpuId(PVM pVM, PCPUMCTXCORE pRegFrame)
{
    /* Note: operates the same in 64 and non-64 bits mode. */
    CPUMGetGuestCpuId(pVM, pRegFrame->eax, &pRegFrame->eax, &pRegFrame->ebx, &pRegFrame->ecx, &pRegFrame->edx);
    return VINF_SUCCESS;
}

static int emInterpretCpuId(PVM pVM, PDISCPUSTATE pCpu, PCPUMCTXCORE pRegFrame, RTGCPTR pvFault, uint32_t *pcbSize)
{
    uint32_t iLeaf = pRegFrame->eax; NOREF(iLeaf);

    int rc = EMInterpretCpuId(pVM, pRegFrame);
    Log(("Emulate: CPUID %x -> %08x %08x %08x %08x\n", iLeaf, pRegFrame->eax, pRegFrame->ebx, pRegFrame->ecx, pRegFrame->edx));
    return rc;
}

/**
 * MOV CRx Emulation.
 */

/**
 * Interpret CRx read
 *
 * @returns VBox status code.
 * @param   pVM         The VM handle.
 * @param   pRegFrame   The register frame.
 * @param   DestRegGen  General purpose register index (USE_REG_E**))
 * @param   SrcRegCRx   CRx register index (USE_REG_CR*)
 *
 */
EMDECL(int) EMInterpretCRxRead(PVM pVM, PCPUMCTXCORE pRegFrame, uint32_t DestRegGen, uint32_t SrcRegCrx)
{
    uint64_t val64;

    int rc = CPUMGetGuestCRx(pVM, SrcRegCrx, &val64);
    AssertMsgRCReturn(rc, ("CPUMGetGuestCRx %d failed\n", SrcRegCrx), VERR_EM_INTERPRETER);

    if (CPUMIsGuestIn64BitCode(pVM, pRegFrame))
        rc = DISWriteReg64(pRegFrame, DestRegGen, val64);
    else
        rc = DISWriteReg32(pRegFrame, DestRegGen, val64);

    if(VBOX_SUCCESS(rc))
    {
        LogFlow(("MOV_CR: gen32=%d CR=%d val=%VX64\n", DestRegGen, SrcRegCrx, val64));
        return VINF_SUCCESS;
    }
    return VERR_EM_INTERPRETER;
}


/**
 * Interpret LMSW
 *
 * @returns VBox status code.
 * @param   pVM         The VM handle.
 * @param   u16Data     LMSW source data.
 *
 */
EMDECL(int) EMInterpretLMSW(PVM pVM, uint16_t u16Data)
{
    uint64_t OldCr0 = CPUMGetGuestCR0(pVM);

    /* don't use this path to go into protected mode! */
    Assert(OldCr0 & X86_CR0_PE);
    if (!(OldCr0 & X86_CR0_PE))
        return VERR_EM_INTERPRETER;

    /* Only PE, MP, EM and TS can be changed; note that PE can't be cleared by this instruction. */
    uint64_t NewCr0 = ( OldCr0 & ~(             X86_CR0_MP | X86_CR0_EM | X86_CR0_TS))
                    | (u16Data &  (X86_CR0_PE | X86_CR0_MP | X86_CR0_EM | X86_CR0_TS));

#ifdef IN_GC
    /* Need to change the hyper CR0? Doing it the lazy way then. */
    if (    (OldCr0 & (X86_CR0_AM | X86_CR0_WP))
        !=  (NewCr0 & (X86_CR0_AM | X86_CR0_WP)))
    {
        Log(("EMInterpretLMSW: CR0: %#x->%#x => R3\n", OldCr0, NewCr0));
        VM_FF_SET(pVM, VM_FF_TO_R3);
    }
#endif

    return CPUMSetGuestCR0(pVM, NewCr0);
}


/**
 * Interpret CLTS
 *
 * @returns VBox status code.
 * @param   pVM         The VM handle.
 *
 */
EMDECL(int) EMInterpretCLTS(PVM pVM)
{
    uint64_t cr0 = CPUMGetGuestCR0(pVM);
    if (!(cr0 & X86_CR0_TS))
        return VINF_SUCCESS;
    return CPUMSetGuestCR0(pVM, cr0 & ~X86_CR0_TS);
}

static int emInterpretClts(PVM pVM, PDISCPUSTATE pCpu, PCPUMCTXCORE pRegFrame, RTGCPTR pvFault, uint32_t *pcbSize)
{
    return EMInterpretCLTS(pVM);
}

/**
 * Interpret CRx write
 *
 * @returns VBox status code.
 * @param   pVM         The VM handle.
 * @param   pRegFrame   The register frame.
 * @param   DestRegCRx  CRx register index (USE_REG_CR*)
 * @param   SrcRegGen   General purpose register index (USE_REG_E**))
 *
 */
EMDECL(int) EMInterpretCRxWrite(PVM pVM, PCPUMCTXCORE pRegFrame, uint32_t DestRegCrx, uint32_t SrcRegGen)
{
    uint64_t val;
    uint64_t oldval;
    uint64_t msrEFER;
    int      rc;

    /** @todo Clean up this mess. */
    if (CPUMIsGuestIn64BitCode(pVM, pRegFrame))
    {
        rc = DISFetchReg64(pRegFrame, SrcRegGen, &val);
    }
    else 
    {
        uint32_t val32;
        rc = DISFetchReg32(pRegFrame, SrcRegGen, &val32);
        val = val32;
    }

    if (VBOX_SUCCESS(rc))
    {
        switch (DestRegCrx)
        {
        case USE_REG_CR0:
            oldval = CPUMGetGuestCR0(pVM);
#ifdef IN_GC
            /* CR0.WP and CR0.AM changes require a reschedule run in ring 3. */
            if (    (val    & (X86_CR0_WP | X86_CR0_AM)) 
                !=  (oldval & (X86_CR0_WP | X86_CR0_AM)))
                return VERR_EM_INTERPRETER;
#endif
            CPUMSetGuestCR0(pVM, val);
            val = CPUMGetGuestCR0(pVM);
            if (    (oldval & (X86_CR0_PG | X86_CR0_WP | X86_CR0_PE))
                !=  (val    & (X86_CR0_PG | X86_CR0_WP | X86_CR0_PE)))
            {
                /* global flush */
                rc = PGMFlushTLB(pVM, CPUMGetGuestCR3(pVM), true /* global */);
                AssertRCReturn(rc, rc);
            }

            /* Deal with long mode enabling/disabling. */
            msrEFER = CPUMGetGuestEFER(pVM);
            if (msrEFER & MSR_K6_EFER_LME)
            {
                if (    !(oldval & X86_CR0_PG)
                    &&  (val & X86_CR0_PG))
                {
                    /* Illegal to have an active 64 bits CS selector (AMD Arch. Programmer's Manual Volume 2: Table 14-5) */
                    if (pRegFrame->csHid.Attr.n.u1Long)
                    {
                        AssertMsgFailed(("Illegal enabling of paging with CS.u1Long = 1!!\n"));
                        return VERR_EM_INTERPRETER; /* @todo generate #GP(0) */
                    }

                    /* Illegal to switch to long mode before activating PAE first (AMD Arch. Programmer's Manual Volume 2: Table 14-5) */
                    if (!(CPUMGetGuestCR4(pVM) & X86_CR4_PAE))
                    {
                        AssertMsgFailed(("Illegal enabling of paging with PAE disabled!!\n"));
                        return VERR_EM_INTERPRETER; /* @todo generate #GP(0) */
                    }

                    msrEFER |= MSR_K6_EFER_LMA;
                }
                else
                if (    (oldval & X86_CR0_PG)
                    &&  !(val & X86_CR0_PG))
                {
                    msrEFER &= ~MSR_K6_EFER_LMA;
                    /* @todo Do we need to cut off rip here? High dword of rip is undefined, so it shouldn't really matter. */
                }
                CPUMSetGuestEFER(pVM, msrEFER);
            }
            return PGMChangeMode(pVM, CPUMGetGuestCR0(pVM), CPUMGetGuestCR4(pVM), CPUMGetGuestEFER(pVM));

        case USE_REG_CR2:
            rc = CPUMSetGuestCR2(pVM, val); AssertRC(rc);
            return VINF_SUCCESS;

        case USE_REG_CR3:
            /* Reloading the current CR3 means the guest just wants to flush the TLBs */
            rc = CPUMSetGuestCR3(pVM, val); AssertRC(rc);
            if (CPUMGetGuestCR0(pVM) & X86_CR0_PG)
            {
                /* flush */
                rc = PGMFlushTLB(pVM, val, !(CPUMGetGuestCR4(pVM) & X86_CR4_PGE));
                AssertRCReturn(rc, rc);
            }
            return VINF_SUCCESS;

        case USE_REG_CR4:
            oldval = CPUMGetGuestCR4(pVM);
            rc = CPUMSetGuestCR4(pVM, val); AssertRC(rc);
            val   = CPUMGetGuestCR4(pVM);

            msrEFER = CPUMGetGuestEFER(pVM);
            /* Illegal to disable PAE when long mode is active. (AMD Arch. Programmer's Manual Volume 2: Table 14-5) */
            if (    (msrEFER & MSR_K6_EFER_LMA)
                &&  (oldval & X86_CR4_PAE)
                &&  !(val & X86_CR4_PAE))
            {
                return VERR_EM_INTERPRETER; /* @todo generate #GP(0) */
            }

            if (    (oldval & (X86_CR4_PGE|X86_CR4_PAE|X86_CR4_PSE))
                !=  (val    & (X86_CR4_PGE|X86_CR4_PAE|X86_CR4_PSE)))
            {
                /* global flush */
                rc = PGMFlushTLB(pVM, CPUMGetGuestCR3(pVM), true /* global */);
                AssertRCReturn(rc, rc);
            }
# ifdef IN_GC
            /* Feeling extremely lazy. */
            if (    (oldval & (X86_CR4_OSFSXR|X86_CR4_OSXMMEEXCPT|X86_CR4_PCE|X86_CR4_MCE|X86_CR4_PAE|X86_CR4_DE|X86_CR4_TSD|X86_CR4_PVI|X86_CR4_VME))
                !=  (val    & (X86_CR4_OSFSXR|X86_CR4_OSXMMEEXCPT|X86_CR4_PCE|X86_CR4_MCE|X86_CR4_PAE|X86_CR4_DE|X86_CR4_TSD|X86_CR4_PVI|X86_CR4_VME)))
            {
                Log(("emInterpretMovCRx: CR4: %#RX64->%#RX64 => R3\n", oldval, val));
                VM_FF_SET(pVM, VM_FF_TO_R3);
            }
# endif
            return PGMChangeMode(pVM, CPUMGetGuestCR0(pVM), CPUMGetGuestCR4(pVM), CPUMGetGuestEFER(pVM));

        default:
            AssertFailed();
        case USE_REG_CR1: /* illegal op */
            break;
        }
    }
    return VERR_EM_INTERPRETER;
}

static int emInterpretMovCRx(PVM pVM, PDISCPUSTATE pCpu, PCPUMCTXCORE pRegFrame, RTGCPTR pvFault, uint32_t *pcbSize)
{
    if ((pCpu->param1.flags == USE_REG_GEN32 || pCpu->param1.flags == USE_REG_GEN64) && pCpu->param2.flags == USE_REG_CR)
        return EMInterpretCRxRead(pVM, pRegFrame, pCpu->param1.base.reg_gen, pCpu->param2.base.reg_ctrl);

    if (pCpu->param1.flags == USE_REG_CR && (pCpu->param2.flags == USE_REG_GEN32 || pCpu->param2.flags == USE_REG_GEN64))
        return EMInterpretCRxWrite(pVM, pRegFrame, pCpu->param1.base.reg_ctrl, pCpu->param2.base.reg_gen);

    AssertMsgFailedReturn(("Unexpected control register move\n"), VERR_EM_INTERPRETER);
    return VERR_EM_INTERPRETER;
}

/**
 * MOV DRx
 */

/**
 * Interpret DRx write
 *
 * @returns VBox status code.
 * @param   pVM         The VM handle.
 * @param   pRegFrame   The register frame.
 * @param   DestRegDRx  DRx register index (USE_REG_DR*)
 * @param   SrcRegGen   General purpose register index (USE_REG_E**))
 *
 */
EMDECL(int) EMInterpretDRxWrite(PVM pVM, PCPUMCTXCORE pRegFrame, uint32_t DestRegDrx, uint32_t SrcRegGen)
{
    uint64_t val;
    int      rc;

    if (CPUMIsGuestIn64BitCode(pVM, pRegFrame))
    {
        rc = DISFetchReg64(pRegFrame, SrcRegGen, &val);
    }
    else 
    {
        uint32_t val32;
        rc = DISFetchReg32(pRegFrame, SrcRegGen, &val32);
        val = val32;
    }

    if (VBOX_SUCCESS(rc))
    {
        rc = CPUMSetGuestDRx(pVM, DestRegDrx, val);
        if (VBOX_SUCCESS(rc))
            return rc;
        AssertMsgFailed(("CPUMSetGuestDRx %d failed\n", DestRegDrx));
    }
    return VERR_EM_INTERPRETER;
}

/**
 * Interpret DRx read
 *
 * @returns VBox status code.
 * @param   pVM         The VM handle.
 * @param   pRegFrame   The register frame.
 * @param   DestRegGen  General purpose register index (USE_REG_E**))
 * @param   SrcRegDRx   DRx register index (USE_REG_DR*)
 *
 */
EMDECL(int) EMInterpretDRxRead(PVM pVM, PCPUMCTXCORE pRegFrame, uint32_t DestRegGen, uint32_t SrcRegDrx)
{
    uint64_t val64;

    int rc = CPUMGetGuestDRx(pVM, SrcRegDrx, &val64);
    AssertMsgRCReturn(rc, ("CPUMGetGuestDRx %d failed\n", SrcRegDrx), VERR_EM_INTERPRETER);
    if (CPUMIsGuestIn64BitCode(pVM, pRegFrame))
    {
        rc = DISWriteReg64(pRegFrame, DestRegGen, val64);
    }
    else
        rc = DISWriteReg32(pRegFrame, DestRegGen, (uint32_t)val64);

    if (VBOX_SUCCESS(rc))
        return VINF_SUCCESS;

    return VERR_EM_INTERPRETER;
}

static int emInterpretMovDRx(PVM pVM, PDISCPUSTATE pCpu, PCPUMCTXCORE pRegFrame, RTGCPTR pvFault, uint32_t *pcbSize)
{
    int rc = VERR_EM_INTERPRETER;

    if((pCpu->param1.flags == USE_REG_GEN32 || pCpu->param1.flags == USE_REG_GEN64) && pCpu->param2.flags == USE_REG_DBG)
    {
        rc = EMInterpretDRxRead(pVM, pRegFrame, pCpu->param1.base.reg_gen, pCpu->param2.base.reg_dbg);
    }
    else
    if(pCpu->param1.flags == USE_REG_DBG && (pCpu->param2.flags == USE_REG_GEN32 || pCpu->param2.flags == USE_REG_GEN64))
    {
        rc = EMInterpretDRxWrite(pVM, pRegFrame, pCpu->param1.base.reg_dbg, pCpu->param2.base.reg_gen);
    }
    else
        AssertMsgFailed(("Unexpected debug register move\n"));

    return rc;
}

/**
 * LLDT Emulation.
 */
static int emInterpretLLdt(PVM pVM, PDISCPUSTATE pCpu, PCPUMCTXCORE pRegFrame, RTGCPTR pvFault, uint32_t *pcbSize)
{
    OP_PARAMVAL param1;
    RTSEL       sel;

    int rc = DISQueryParamVal(pRegFrame, pCpu, &pCpu->param1, &param1, PARAM_SOURCE);
    if(VBOX_FAILURE(rc))
        return VERR_EM_INTERPRETER;

    switch(param1.type)
    {
    case PARMTYPE_ADDRESS:
        return VERR_EM_INTERPRETER; //feeling lazy right now

    case PARMTYPE_IMMEDIATE:
        if(!(param1.flags & PARAM_VAL16))
            return VERR_EM_INTERPRETER;
        sel = (RTSEL)param1.val.val16;
        break;

    default:
        return VERR_EM_INTERPRETER;
    }

    if (sel == 0)
    {
        if (CPUMGetHyperLDTR(pVM) == 0)
        {
            // this simple case is most frequent in Windows 2000 (31k - boot & shutdown)
            return VINF_SUCCESS;
        }
    }
    //still feeling lazy
    return VERR_EM_INTERPRETER;
}

#ifdef IN_GC
/**
 * STI Emulation.
 *
 * @remark the instruction following sti is guaranteed to be executed before any interrupts are dispatched
 */
static int emInterpretSti(PVM pVM, PDISCPUSTATE pCpu, PCPUMCTXCORE pRegFrame, RTGCPTR pvFault, uint32_t *pcbSize)
{
    PPATMGCSTATE pGCState = PATMQueryGCState(pVM);

    if(!pGCState)
    {
        Assert(pGCState);
        return VERR_EM_INTERPRETER;
    }
    pGCState->uVMFlags |= X86_EFL_IF;

    Assert(pRegFrame->eflags.u32 & X86_EFL_IF);
    Assert(pvFault == SELMToFlat(pVM, DIS_SELREG_CS, pRegFrame, (RTGCPTR)pRegFrame->rip));

    pVM->em.s.GCPtrInhibitInterrupts = pRegFrame->eip + pCpu->opsize;
    VM_FF_SET(pVM, VM_FF_INHIBIT_INTERRUPTS);

    return VINF_SUCCESS;
}
#endif /* IN_GC */


/**
 * HLT Emulation.
 */
static int emInterpretHlt(PVM pVM, PDISCPUSTATE pCpu, PCPUMCTXCORE pRegFrame, RTGCPTR pvFault, uint32_t *pcbSize)
{
    return VINF_EM_HALT;
}


/**
 * RDTSC Emulation.
 */

/**
 * Interpret RDTSC
 *
 * @returns VBox status code.
 * @param   pVM         The VM handle.
 * @param   pRegFrame   The register frame.
 *
 */
EMDECL(int) EMInterpretRdtsc(PVM pVM, PCPUMCTXCORE pRegFrame)
{
    unsigned uCR4 = CPUMGetGuestCR4(pVM);

    if (uCR4 & X86_CR4_TSD)
        return VERR_EM_INTERPRETER; /* genuine #GP */

    uint64_t uTicks = TMCpuTickGet(pVM);

    /* Same behaviour in 32 & 64 bits mode */
    pRegFrame->eax = uTicks;
    pRegFrame->edx = (uTicks >> 32ULL);

    return VINF_SUCCESS;
}

static int emInterpretRdtsc(PVM pVM, PDISCPUSTATE pCpu, PCPUMCTXCORE pRegFrame, RTGCPTR pvFault, uint32_t *pcbSize)
{
    return EMInterpretRdtsc(pVM, pRegFrame);
}

/**
 * MONITOR Emulation.
 */
static int emInterpretMonitor(PVM pVM, PDISCPUSTATE pCpu, PCPUMCTXCORE pRegFrame, RTGCPTR pvFault, uint32_t *pcbSize)
{
    uint32_t u32Dummy, u32ExtFeatures, cpl;

    Assert(pCpu->mode != CPUMODE_64BIT);    /** @todo check */
    if (pRegFrame->ecx != 0)
        return VERR_EM_INTERPRETER; /* illegal value. */

    /* Get the current privilege level. */
    cpl = CPUMGetGuestCPL(pVM, pRegFrame);
    if (cpl != 0)
        return VERR_EM_INTERPRETER; /* supervisor only */

    CPUMGetGuestCpuId(pVM, 1, &u32Dummy, &u32Dummy, &u32ExtFeatures, &u32Dummy);
    if (!(u32ExtFeatures & X86_CPUID_FEATURE_ECX_MONITOR))
        return VERR_EM_INTERPRETER; /* not supported */

    return VINF_SUCCESS;
}


/**
 * MWAIT Emulation.
 */
static int emInterpretMWait(PVM pVM, PDISCPUSTATE pCpu, PCPUMCTXCORE pRegFrame, RTGCPTR pvFault, uint32_t *pcbSize)
{
    uint32_t u32Dummy, u32ExtFeatures, cpl;

    Assert(pCpu->mode != CPUMODE_64BIT);    /** @todo check */
    if (pRegFrame->ecx != 0)
        return VERR_EM_INTERPRETER; /* illegal value. */

    /* Get the current privilege level. */
    cpl = CPUMGetGuestCPL(pVM, pRegFrame);
    if (cpl != 0)
        return VERR_EM_INTERPRETER; /* supervisor only */

    CPUMGetGuestCpuId(pVM, 1, &u32Dummy, &u32Dummy, &u32ExtFeatures, &u32Dummy);
    if (!(u32ExtFeatures & X86_CPUID_FEATURE_ECX_MONITOR))
        return VERR_EM_INTERPRETER; /* not supported */

    /** @todo not completely correct */
    return VINF_EM_HALT;
}

/**
 * Interpret RDMSR
 *
 * @returns VBox status code.
 * @param   pVM         The VM handle.
 * @param   pRegFrame   The register frame.
 *
 */
EMDECL(int) EMInterpretRdmsr(PVM pVM, PCPUMCTXCORE pRegFrame)
{
    uint32_t u32Dummy, u32Features, cpl;
    uint64_t val;
    CPUMCTX *pCtx;
    int      rc;

    /** @todo According to the Intel manuals, there's a REX version of RDMSR that is slightly different.
     *  That version clears the high dwords of both RDX & RAX */
    rc = CPUMQueryGuestCtxPtr(pVM, &pCtx);
    AssertRC(rc);

    /* Get the current privilege level. */
    cpl = CPUMGetGuestCPL(pVM, pRegFrame);
    if (cpl != 0)
        return VERR_EM_INTERPRETER; /* supervisor only */

    CPUMGetGuestCpuId(pVM, 1, &u32Dummy, &u32Dummy, &u32Dummy, &u32Features);
    if (!(u32Features & X86_CPUID_FEATURE_EDX_MSR))
        return VERR_EM_INTERPRETER; /* not supported */

    switch (pRegFrame->ecx)
    {
    case MSR_IA32_APICBASE:
        rc = PDMApicGetBase(pVM, &val);
        AssertRC(rc);
        break;

    case MSR_IA32_CR_PAT:
        val = pCtx->msrPAT;
        break;

    case MSR_IA32_SYSENTER_CS:
        val = pCtx->SysEnter.cs;
        break;

    case MSR_IA32_SYSENTER_EIP:
        val = pCtx->SysEnter.eip;
        break;

    case MSR_IA32_SYSENTER_ESP:
        val = pCtx->SysEnter.esp;
        break;

    case MSR_K6_EFER:
        val = pCtx->msrEFER;
        break;

    case MSR_K8_SF_MASK:
        val = pCtx->msrSFMASK;
        break;

    case MSR_K6_STAR:
        val = pCtx->msrSTAR;
        break;

    case MSR_K8_LSTAR:
        val = pCtx->msrLSTAR;
        break;

    case MSR_K8_CSTAR:
        val = pCtx->msrCSTAR;
        break;

    case MSR_K8_FS_BASE:
        val = pCtx->fsHid.u64Base;
        break;

    case MSR_K8_GS_BASE:
        val = pCtx->gsHid.u64Base;
        break;

    case MSR_K8_KERNEL_GS_BASE:
        val = pCtx->msrKERNELGSBASE;
        break;

    default:
        /* We should actually trigger a #GP here, but don't as that might cause more trouble. */
        val = 0;
        break;
    }
    Log(("EMInterpretRdmsr %x -> val=%VX64\n", pRegFrame->ecx, val));
    pRegFrame->eax = (uint32_t) val;
    pRegFrame->edx = (uint32_t) (val >> 32ULL);
    return VINF_SUCCESS;
}

/**
 * RDMSR Emulation.
 */
static int emInterpretRdmsr(PVM pVM, PDISCPUSTATE pCpu, PCPUMCTXCORE pRegFrame, RTGCPTR pvFault, uint32_t *pcbSize)
{
    /* Note: the intel manual claims there's a REX version of RDMSR that's slightly different, so we play safe by completely disassembling the instruction. */
    Assert(!(pCpu->prefix & PREFIX_REX));
    return EMInterpretRdmsr(pVM, pRegFrame);
}

/**
 * Interpret WRMSR
 *
 * @returns VBox status code.
 * @param   pVM         The VM handle.
 * @param   pRegFrame   The register frame.
 *
 */
EMDECL(int) EMInterpretWrmsr(PVM pVM, PCPUMCTXCORE pRegFrame)
{
    uint32_t u32Dummy, u32Features, cpl;
    uint64_t val;
    CPUMCTX *pCtx;
    int      rc;

    /* Note: works the same in 32 and 64 bits modes. */
    rc = CPUMQueryGuestCtxPtr(pVM, &pCtx);
    AssertRC(rc);

    /* Get the current privilege level. */
    cpl = CPUMGetGuestCPL(pVM, pRegFrame);
    if (cpl != 0)
        return VERR_EM_INTERPRETER; /* supervisor only */

    CPUMGetGuestCpuId(pVM, 1, &u32Dummy, &u32Dummy, &u32Dummy, &u32Features);
    if (!(u32Features & X86_CPUID_FEATURE_EDX_MSR))
        return VERR_EM_INTERPRETER; /* not supported */

    val = (uint64_t)pRegFrame->eax | ((uint64_t)pRegFrame->edx << 32ULL);
    Log(("EMInterpretWrmsr %x val=%VX64\n", pRegFrame->ecx, val));
    switch (pRegFrame->ecx)
    {
    case MSR_IA32_APICBASE:
        rc = PDMApicSetBase(pVM, val);
        AssertRC(rc);
        break;

    case MSR_IA32_CR_PAT:
        pCtx->msrPAT = val;
        break;

    case MSR_IA32_SYSENTER_CS:
        pCtx->SysEnter.cs = val;
        break;

    case MSR_IA32_SYSENTER_EIP:
        pCtx->SysEnter.eip = val;
        break;

    case MSR_IA32_SYSENTER_ESP:
        pCtx->SysEnter.esp = val;
        break;

    case MSR_K6_EFER:
    {
        uint64_t uMask = 0;

        /* Filter out those bits the guest is allowed to change. (e.g. LMA is read-only) */
        CPUMGetGuestCpuId(pVM, 0x80000001, &u32Dummy, &u32Dummy, &u32Dummy, &u32Features);
        if (u32Features & X86_CPUID_AMD_FEATURE_EDX_NX)
            uMask |= MSR_K6_EFER_NXE;
        if (u32Features & X86_CPUID_AMD_FEATURE_EDX_LONG_MODE)
            uMask |= MSR_K6_EFER_LME;
        if (u32Features & X86_CPUID_AMD_FEATURE_EDX_SEP)
            uMask |= MSR_K6_EFER_SCE;

        /* Check for illegal MSR_K6_EFER_LME transitions: not allowed to change LME if paging is enabled. (AMD Arch. Programmer's Manual Volume 2: Table 14-5) */
        if (    ((pCtx->msrEFER & MSR_K6_EFER_LME) != (val & uMask & MSR_K6_EFER_LME))
            &&  (pCtx->cr0 & X86_CR0_PG))
        {
            AssertMsgFailed(("Illegal MSR_K6_EFER_LME change: paging is enabled!!\n"));
            return VERR_EM_INTERPRETER; /* @todo generate #GP(0) */
        }

        /* There are a few more: e.g. MSR_K6_EFER_FFXSR, MSR_K6_EFER_LMSLE */
        AssertMsg(!(val & ~(MSR_K6_EFER_NXE|MSR_K6_EFER_LME|MSR_K6_EFER_LMA /* ignored anyway */ |MSR_K6_EFER_SCE)), ("Unexpected value %RX64\n", val));
        pCtx->msrEFER = (pCtx->msrEFER & ~uMask) | (val & uMask);
        break;
    }

    case MSR_K8_SF_MASK:
        pCtx->msrSFMASK = val;
        break;

    case MSR_K6_STAR:
        pCtx->msrSTAR = val;
        break;

    case MSR_K8_LSTAR:
        pCtx->msrLSTAR = val;
        break;

    case MSR_K8_CSTAR:
        pCtx->msrCSTAR = val;
        break;

    case MSR_K8_FS_BASE:
        pCtx->fsHid.u64Base = val;
        break;

    case MSR_K8_GS_BASE:
        pCtx->gsHid.u64Base = val;
        break;

    case MSR_K8_KERNEL_GS_BASE:
        pCtx->msrKERNELGSBASE = val;
        break;

    default:
        /* We should actually trigger a #GP here, but don't as that might cause more trouble. */
        break;
    }
    return VINF_SUCCESS;
}

/**
 * WRMSR Emulation.
 */
static int emInterpretWrmsr(PVM pVM, PDISCPUSTATE pCpu, PCPUMCTXCORE pRegFrame, RTGCPTR pvFault, uint32_t *pcbSize)
{
    return EMInterpretWrmsr(pVM, pRegFrame);
}

/**
 * Internal worker.
 * @copydoc EMInterpretInstructionCPU
 */
DECLINLINE(int) emInterpretInstructionCPU(PVM pVM, PDISCPUSTATE pCpu, PCPUMCTXCORE pRegFrame, RTGCPTR pvFault, uint32_t *pcbSize)
{
    Assert(pcbSize);
    *pcbSize = 0;

    /*
     * Only supervisor guest code!!
     * And no complicated prefixes.
     */
    /* Get the current privilege level. */
    uint32_t cpl = CPUMGetGuestCPL(pVM, pRegFrame);
    if (    cpl != 0
        &&  pCpu->pCurInstr->opcode != OP_RDTSC)    /* rdtsc requires emulation in ring 3 as well */
    {
        Log(("WARNING: refusing instruction emulation for user-mode code!!\n"));
        STAM_COUNTER_INC(&pVM->em.s.CTXSUFF(pStats)->CTXMID(Stat,FailedUserMode));
        return VERR_EM_INTERPRETER;
    }

#ifdef IN_GC
    if (    (pCpu->prefix & (PREFIX_REPNE | PREFIX_REP))
        ||  (   (pCpu->prefix & PREFIX_LOCK)
             && pCpu->pCurInstr->opcode != OP_CMPXCHG
             && pCpu->pCurInstr->opcode != OP_CMPXCHG8B
             && pCpu->pCurInstr->opcode != OP_XADD
             && pCpu->pCurInstr->opcode != OP_OR
             && pCpu->pCurInstr->opcode != OP_BTR
            )
       )
#else
    if (    (pCpu->prefix & (PREFIX_REPNE | PREFIX_REP))
        ||  (   (pCpu->prefix & PREFIX_LOCK)
             && pCpu->pCurInstr->opcode != OP_OR
             && pCpu->pCurInstr->opcode != OP_BTR
            )
       )
#endif
    {
        //Log(("EMInterpretInstruction: wrong prefix!!\n"));
        STAM_COUNTER_INC(&pVM->em.s.CTXSUFF(pStats)->CTXMID(Stat,FailedPrefix));
        return VERR_EM_INTERPRETER;
    }

    int rc;
#if defined(IN_GC) && (defined(VBOX_STRICT) || defined(LOG_ENABLED))
    LogFlow(("emInterpretInstructionCPU %s\n", emGetMnemonic(pCpu)));
#endif
    switch (pCpu->pCurInstr->opcode)
    {
# define INTERPRET_CASE_EX_LOCK_PARAM3(opcode, Instr, InstrFn, pfnEmulate, pfnEmulateLock) \
        case opcode:\
            if (pCpu->prefix & PREFIX_LOCK) \
                rc = emInterpretLock##InstrFn(pVM, pCpu, pRegFrame, pvFault, pcbSize, pfnEmulateLock); \
            else \
                rc = emInterpret##InstrFn(pVM, pCpu, pRegFrame, pvFault, pcbSize, pfnEmulate); \
            if (VBOX_SUCCESS(rc)) \
                STAM_COUNTER_INC(&pVM->em.s.CTXSUFF(pStats)->CTXMID(Stat,Instr)); \
            else \
                STAM_COUNTER_INC(&pVM->em.s.CTXSUFF(pStats)->CTXMID(Stat,Failed##Instr)); \
            return rc
#define INTERPRET_CASE_EX_PARAM3(opcode, Instr, InstrFn, pfnEmulate) \
        case opcode:\
            rc = emInterpret##InstrFn(pVM, pCpu, pRegFrame, pvFault, pcbSize, pfnEmulate); \
            if (VBOX_SUCCESS(rc)) \
                STAM_COUNTER_INC(&pVM->em.s.CTXSUFF(pStats)->CTXMID(Stat,Instr)); \
            else \
                STAM_COUNTER_INC(&pVM->em.s.CTXSUFF(pStats)->CTXMID(Stat,Failed##Instr)); \
            return rc

#define INTERPRET_CASE_EX_PARAM2(opcode, Instr, InstrFn, pfnEmulate) \
            INTERPRET_CASE_EX_PARAM3(opcode, Instr, InstrFn, pfnEmulate)
#define INTERPRET_CASE_EX_LOCK_PARAM2(opcode, Instr, InstrFn, pfnEmulate, pfnEmulateLock) \
            INTERPRET_CASE_EX_LOCK_PARAM3(opcode, Instr, InstrFn, pfnEmulate, pfnEmulateLock)

#define INTERPRET_CASE(opcode, Instr) \
        case opcode:\
            rc = emInterpret##Instr(pVM, pCpu, pRegFrame, pvFault, pcbSize); \
            if (VBOX_SUCCESS(rc)) \
                STAM_COUNTER_INC(&pVM->em.s.CTXSUFF(pStats)->CTXMID(Stat,Instr)); \
            else \
                STAM_COUNTER_INC(&pVM->em.s.CTXSUFF(pStats)->CTXMID(Stat,Failed##Instr)); \
            return rc
#define INTERPRET_STAT_CASE(opcode, Instr) \
        case opcode: STAM_COUNTER_INC(&pVM->em.s.CTXSUFF(pStats)->CTXMID(Stat,Failed##Instr)); return VERR_EM_INTERPRETER;

        INTERPRET_CASE(OP_XCHG,Xchg);
        INTERPRET_CASE_EX_PARAM2(OP_DEC,Dec, IncDec, EMEmulateDec);
        INTERPRET_CASE_EX_PARAM2(OP_INC,Inc, IncDec, EMEmulateInc);
        INTERPRET_CASE(OP_POP,Pop);
        INTERPRET_CASE_EX_LOCK_PARAM3(OP_OR, Or, OrXorAnd, EMEmulateOr, EMEmulateLockOr);
        INTERPRET_CASE_EX_PARAM3(OP_XOR,Xor, OrXorAnd, EMEmulateXor);
        INTERPRET_CASE_EX_PARAM3(OP_AND,And, OrXorAnd, EMEmulateAnd);
        INTERPRET_CASE(OP_MOV,Mov);
        INTERPRET_CASE(OP_INVLPG,InvlPg);
        INTERPRET_CASE(OP_CPUID,CpuId);
        INTERPRET_CASE(OP_MOV_CR,MovCRx);
        INTERPRET_CASE(OP_MOV_DR,MovDRx);
        INTERPRET_CASE(OP_LLDT,LLdt);
        INTERPRET_CASE(OP_CLTS,Clts);
        INTERPRET_CASE(OP_MONITOR, Monitor);
        INTERPRET_CASE(OP_MWAIT, MWait);
#ifdef VBOX_WITH_MSR_EMULATION
        INTERPRET_CASE(OP_RDMSR, Rdmsr);
        INTERPRET_CASE(OP_WRMSR, Wrmsr);
#endif
        INTERPRET_CASE_EX_PARAM3(OP_ADD,Add, AddSub, EMEmulateAdd);
        INTERPRET_CASE_EX_PARAM3(OP_SUB,Sub, AddSub, EMEmulateSub);
        INTERPRET_CASE(OP_ADC,Adc);
        INTERPRET_CASE_EX_LOCK_PARAM2(OP_BTR,Btr, BitTest, EMEmulateBtr, EMEmulateLockBtr);
        INTERPRET_CASE_EX_PARAM2(OP_BTS,Bts, BitTest, EMEmulateBts);
        INTERPRET_CASE_EX_PARAM2(OP_BTC,Btc, BitTest, EMEmulateBtc);
        INTERPRET_CASE(OP_RDTSC,Rdtsc);
#ifdef IN_GC
        INTERPRET_CASE(OP_STI,Sti);
        INTERPRET_CASE(OP_CMPXCHG, CmpXchg);
        INTERPRET_CASE(OP_CMPXCHG8B, CmpXchg8b);
        INTERPRET_CASE(OP_XADD, XAdd);
#endif
        INTERPRET_CASE(OP_HLT,Hlt);
        INTERPRET_CASE(OP_IRET,Iret);
#ifdef VBOX_WITH_STATISTICS
#ifndef IN_GC
        INTERPRET_STAT_CASE(OP_CMPXCHG,CmpXchg);
        INTERPRET_STAT_CASE(OP_CMPXCHG8B, CmpXchg8b);
        INTERPRET_STAT_CASE(OP_XADD, XAdd);
#endif
        INTERPRET_STAT_CASE(OP_MOVNTPS,MovNTPS);
        INTERPRET_STAT_CASE(OP_STOSWD,StosWD);
        INTERPRET_STAT_CASE(OP_WBINVD,WbInvd);
#endif
        default:
            Log3(("emInterpretInstructionCPU: opcode=%d\n", pCpu->pCurInstr->opcode));
            STAM_COUNTER_INC(&pVM->em.s.CTXSUFF(pStats)->CTXMID(Stat,FailedMisc));
            return VERR_EM_INTERPRETER;
#undef INTERPRET_CASE_EX_PARAM2
#undef INTERPRET_STAT_CASE
#undef INTERPRET_CASE_EX
#undef INTERPRET_CASE
    }
    AssertFailed();
    return VERR_INTERNAL_ERROR;
}


/**
 * Sets the PC for which interrupts should be inhibited.
 *
 * @param   pVM         The VM handle.
 * @param   PC          The PC.
 */
EMDECL(void) EMSetInhibitInterruptsPC(PVM pVM, RTGCUINTPTR PC)
{
    pVM->em.s.GCPtrInhibitInterrupts = PC;
    VM_FF_SET(pVM, VM_FF_INHIBIT_INTERRUPTS);
}


/**
 * Gets the PC for which interrupts should be inhibited.
 *
 * There are a few instructions which inhibits or delays interrupts
 * for the instruction following them. These instructions are:
 *      - STI
 *      - MOV SS, r/m16
 *      - POP SS
 *
 * @returns The PC for which interrupts should be inhibited.
 * @param   pVM         VM handle.
 *
 */
EMDECL(RTGCUINTPTR) EMGetInhibitInterruptsPC(PVM pVM)
{
    return pVM->em.s.GCPtrInhibitInterrupts;
}
