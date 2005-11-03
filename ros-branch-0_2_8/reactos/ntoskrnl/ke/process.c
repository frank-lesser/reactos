/*
 * COPYRIGHT:       See COPYING in the top level directory
 * PROJECT:         ReactOS kernel
 * FILE:            ntoskrnl/ke/process.c
 * PURPOSE:         Attaching/Detaching and System Call Tables
 *
 * PROGRAMMERS:     Alex Ionescu (Implemented Attach/Detach and KeRemoveSystemServiceTable)
 *                  Gregor Anich (Bugfixes to Attach Functions)
 */

/* INCLUDES *****************************************************************/

#include <ntoskrnl.h>
#include <internal/napi.h>
#define NDEBUG
#include <internal/debug.h>

/* GLOBALS   *****************************************************************/

KSERVICE_TABLE_DESCRIPTOR
__declspec(dllexport)
KeServiceDescriptorTable[SSDT_MAX_ENTRIES] = {
    { MainSSDT, NULL, NUMBER_OF_SYSCALLS, MainSSPT },
    { NULL,     NULL,   0,   NULL   },
    { NULL,     NULL,   0,   NULL   },
    { NULL,     NULL,   0,   NULL   }
};

KSERVICE_TABLE_DESCRIPTOR
KeServiceDescriptorTableShadow[SSDT_MAX_ENTRIES] = {
    { MainSSDT, NULL, NUMBER_OF_SYSCALLS, MainSSPT },
    { NULL,     NULL,   0,   NULL   },
    { NULL,     NULL,   0,   NULL   },
    { NULL,     NULL,   0,   NULL   }
};

/* FUNCTIONS *****************************************************************/

static inline void
UpdatePageDirs(PKTHREAD Thread, PKPROCESS Process)
{
    /*
     * The stack and the thread structure of the current process may be
     * located in a page which is not present in the page directory of
     * the process we're attaching to. That would lead to a page fault
     * when this function returns. However, since the processor can't
     * call the page fault handler 'cause it can't push EIP on the stack,
     * this will show up as a stack fault which will crash the entire system.
     * To prevent this, make sure the page directory of the process we're
     * attaching to is up-to-date.
     */
    MmUpdatePageDir((PEPROCESS)Process, (PVOID)Thread->StackLimit, MM_STACK_SIZE);
    MmUpdatePageDir((PEPROCESS)Process, (PVOID)Thread, sizeof(ETHREAD));
}

/*
 * FUNCTION: Returns a pointer to the current process
 */
PKPROCESS
STDCALL
KeGetCurrentProcess(VOID)
{
    return(&(PsGetCurrentProcess()->Pcb));
}

VOID
STDCALL
KeInitializeProcess(PKPROCESS Process,
                    KPRIORITY Priority,
                    KAFFINITY Affinity,
                    LARGE_INTEGER DirectoryTableBase)
{
    DPRINT("KeInitializeProcess. Process: %x, DirectoryTableBase: %x\n", Process, DirectoryTableBase);

    /* Initialize the Dispatcher Header */
    KeInitializeDispatcherHeader(&Process->Header,
                                 ProcessObject,
                                 sizeof(KPROCESS),
                                 FALSE);

    /* Initialize Scheduler Data, Disable Alignment Faults and Set the PDE */
    Process->Affinity = Affinity;
    Process->BasePriority = Priority;
    Process->QuantumReset = 6;
    Process->DirectoryTableBase = DirectoryTableBase;
    Process->AutoAlignment = TRUE;
    Process->IopmOffset = 0xFFFF;
    Process->State = ProcessInMemory;

    /* Initialize the Thread List */
    InitializeListHead(&Process->ThreadListHead);
    KeInitializeSpinLock(&Process->ProcessLock);
    DPRINT("The Process has now been initalized with the Kernel\n");
}

ULONG
STDCALL
KeSetProcess(PKPROCESS Process,
             KPRIORITY Increment)
{
    KIRQL OldIrql;
    ULONG OldState;

    /* Lock Dispatcher */
    OldIrql = KeAcquireDispatcherDatabaseLock();

    /* Get Old State */
    OldState = Process->Header.SignalState;

    /* Signal the Process */
    Process->Header.SignalState = TRUE;
    if ((OldState == 0) && IsListEmpty(&Process->Header.WaitListHead) != TRUE) {

        /* Satisfy waits */
        KiWaitTest((PVOID)Process, Increment);
    }

    /* Release Dispatcher Database */
    KeReleaseDispatcherDatabaseLock(OldIrql);

    /* Return the previous State */
    return OldState;
}

/*
 * @implemented
 */
VOID
STDCALL
KeAttachProcess(PKPROCESS Process)
{
    KIRQL OldIrql;
    PKTHREAD Thread = KeGetCurrentThread();

    DPRINT("KeAttachProcess: %x\n", Process);

    /* Make sure that we are in the right page directory */
    UpdatePageDirs(Thread, Process);

    /* Lock Dispatcher */
    OldIrql = KeAcquireDispatcherDatabaseLock();
    KeAcquireSpinLockAtDpcLevel(&Thread->ApcQueueLock);

    /* Crash system if DPC is being executed! */
    if (KeIsExecutingDpc()) {

        DPRINT1("Invalid attach (Thread is executing a DPC!)\n");
        KEBUGCHECK(INVALID_PROCESS_ATTACH_ATTEMPT);
    }

    /* Check if the Target Process is already attached */
    if (Thread->ApcState.Process == Process || Thread->ApcStateIndex != OriginalApcEnvironment) {

        DPRINT("Process already Attached. Exitting\n");
        KeReleaseSpinLockFromDpcLevel(&Thread->ApcQueueLock);
        KeReleaseDispatcherDatabaseLock(OldIrql);
    } else {

        KiAttachProcess(Thread, Process, OldIrql, &Thread->SavedApcState);
    }
}

VOID
STDCALL
KiAttachProcess(PKTHREAD Thread, PKPROCESS Process, KIRQL ApcLock, PRKAPC_STATE SavedApcState)
{

    DPRINT("KiAttachProcess(Thread: %x, Process: %x, SavedApcState: %x\n", Thread, Process, SavedApcState);

    /* Increase Stack Count */
    Process->StackCount++;

    /* Swap the APC Environment */
    KiMoveApcState(&Thread->ApcState, SavedApcState);

    /* Reinitialize Apc State */
    InitializeListHead(&Thread->ApcState.ApcListHead[KernelMode]);
    InitializeListHead(&Thread->ApcState.ApcListHead[UserMode]);
    Thread->ApcState.Process = Process;
    Thread->ApcState.KernelApcInProgress = FALSE;
    Thread->ApcState.KernelApcPending = FALSE;
    Thread->ApcState.UserApcPending = FALSE;

    /* Update Environment Pointers if needed*/
    if (SavedApcState == &Thread->SavedApcState) {

        Thread->ApcStatePointer[OriginalApcEnvironment] = &Thread->SavedApcState;
        Thread->ApcStatePointer[AttachedApcEnvironment] = &Thread->ApcState;
        Thread->ApcStateIndex = AttachedApcEnvironment;
    }

    /* Swap the Processes */
    DPRINT("Swapping\n");
    KiSwapProcess(Process, SavedApcState->Process);

    /* Return to old IRQL*/
    KeReleaseSpinLockFromDpcLevel(&Thread->ApcQueueLock);
    KeReleaseDispatcherDatabaseLock(ApcLock);

    DPRINT("KiAttachProcess Completed Sucesfully\n");
}

VOID
STDCALL
KiSwapProcess(PKPROCESS NewProcess,
              PKPROCESS OldProcess)
{
    DPRINT("Switching CR3 to: %x\n", NewProcess->DirectoryTableBase.u.LowPart);
    Ke386SetPageTableDirectory(NewProcess->DirectoryTableBase.u.LowPart);
}

/*
 * @implemented
 */
BOOLEAN
STDCALL
KeIsAttachedProcess(VOID)
{
    /* Return the APC State */
    return KeGetCurrentThread()->ApcStateIndex;
}

/*
 * @implemented
 */
VOID
STDCALL
KeStackAttachProcess(IN PKPROCESS Process,
                     OUT PRKAPC_STATE ApcState)
{
    KIRQL OldIrql;
    PKTHREAD Thread = KeGetCurrentThread();

    /* Make sure that we are in the right page directory */
    UpdatePageDirs(Thread, Process);

    OldIrql = KeAcquireDispatcherDatabaseLock();
    KeAcquireSpinLockAtDpcLevel(&Thread->ApcQueueLock);

    /* Crash system if DPC is being executed! */
    if (KeIsExecutingDpc()) {

        DPRINT1("Invalid attach (Thread is executing a DPC!)\n");
        KEBUGCHECK(INVALID_PROCESS_ATTACH_ATTEMPT);
    }

    /* Check if the Target Process is already attached */
    if (Thread->ApcState.Process == Process) {

        ApcState->Process = (PKPROCESS)1;  /* Meaning already attached to the same Process */

    } else {

        /* Check if the Current Thread is already attached and call the Internal Function*/
        if (Thread->ApcStateIndex != OriginalApcEnvironment) {

            KiAttachProcess(Thread, Process, OldIrql, ApcState);
        } else {

            KiAttachProcess(Thread, Process, OldIrql, &Thread->SavedApcState);
            ApcState->Process = NULL;
        }
    }
}

/*
 * @implemented
 */
VOID STDCALL
KeDetachProcess (VOID)
{
    PKTHREAD Thread;
    KIRQL OldIrql;

    DPRINT("KeDetachProcess()\n");

    /* Get Current Thread and Lock */
    Thread = KeGetCurrentThread();
    OldIrql = KeAcquireDispatcherDatabaseLock();
    KeAcquireSpinLockAtDpcLevel(&Thread->ApcQueueLock);

    /* Check if it's attached */
    DPRINT("Current ApcStateIndex: %x\n", Thread->ApcStateIndex);

    if (Thread->ApcStateIndex == OriginalApcEnvironment) {

        DPRINT1("Invalid detach (thread was not attached)\n");
        KEBUGCHECK(INVALID_PROCESS_DETACH_ATTEMPT);
    }

    /* Decrease Stack Count */
    Thread->ApcState.Process->StackCount--;

    /* Restore the APC State */
    KiMoveApcState(&Thread->SavedApcState, &Thread->ApcState);
    Thread->SavedApcState.Process = NULL;
    Thread->ApcStatePointer[OriginalApcEnvironment] = &Thread->ApcState;
    Thread->ApcStatePointer[AttachedApcEnvironment] = &Thread->SavedApcState;
    Thread->ApcStateIndex = OriginalApcEnvironment;

    /* Swap Processes */
    KiSwapProcess(Thread->ApcState.Process, Thread->ApcState.Process);

    /* Unlock Dispatcher */
    KeReleaseSpinLockFromDpcLevel(&Thread->ApcQueueLock);
    KeReleaseDispatcherDatabaseLock(OldIrql);
}

/*
 * @implemented
 */
VOID
STDCALL
KeUnstackDetachProcess (
    IN PRKAPC_STATE ApcState
    )
{
    KIRQL OldIrql;
    PKTHREAD Thread;

    /*
     * If the special "We tried to attach to the process already being
     * attached to" flag is there, don't do anything
     */
    if (ApcState->Process == (PKPROCESS)1) return;

    Thread = KeGetCurrentThread();
    OldIrql = KeAcquireDispatcherDatabaseLock();
    KeAcquireSpinLockAtDpcLevel(&Thread->ApcQueueLock);

    /* Sorry Buddy, can't help you if you've got APCs or just aren't attached */
    if ((Thread->ApcStateIndex == OriginalApcEnvironment) || (Thread->ApcState.KernelApcInProgress)) {

        DPRINT1("Invalid detach (Thread not Attached, or Kernel APC in Progress!)\n");
        KEBUGCHECK(INVALID_PROCESS_DETACH_ATTEMPT);
    }

    /* Restore the Old APC State if a Process was present */
    if (ApcState->Process) {

        KiMoveApcState(ApcState, &Thread->ApcState);

    } else {

        /* The ApcState parameter is useless, so use the saved data and reset it */
        KiMoveApcState(&Thread->SavedApcState, &Thread->ApcState);
        Thread->SavedApcState.Process = NULL;
        Thread->ApcStateIndex = OriginalApcEnvironment;
        Thread->ApcStatePointer[OriginalApcEnvironment] = &Thread->ApcState;
        Thread->ApcStatePointer[AttachedApcEnvironment] = &Thread->SavedApcState;
    }

    /* Swap Processes */
    KiSwapProcess(Thread->ApcState.Process, Thread->ApcState.Process);

    /* Return to old IRQL*/
    KeReleaseSpinLockFromDpcLevel(&Thread->ApcQueueLock);
    KeReleaseDispatcherDatabaseLock(OldIrql);
}

/*
 * @implemented
 */
BOOLEAN
STDCALL
KeAddSystemServiceTable(PULONG_PTR Base,
                        PULONG Count OPTIONAL,
                        ULONG Limit,
                        PUCHAR Number,
                        ULONG Index)
{
    /* Check if descriptor table entry is free */
    if ((Index > SSDT_MAX_ENTRIES - 1) ||
        (KeServiceDescriptorTable[Index].Base) ||
        (KeServiceDescriptorTableShadow[Index].Base))
    {
        return FALSE;
    }

    /* Initialize the shadow service descriptor table */
    KeServiceDescriptorTableShadow[Index].Base = Base;
    KeServiceDescriptorTableShadow[Index].Limit = Limit;
    KeServiceDescriptorTableShadow[Index].Number = Number;
    KeServiceDescriptorTableShadow[Index].Count = Count;

    return TRUE;
}

/*
 * @implemented
 */
BOOLEAN
STDCALL
KeRemoveSystemServiceTable(IN ULONG Index)
{
    /* Make sure the Index is valid */
    if (Index > SSDT_MAX_ENTRIES - 1) return FALSE;

    /* Is there a Normal Descriptor Table? */
    if (!KeServiceDescriptorTable[Index].Base)
    {
        /* Not with the index, is there a shadow at least? */
        if (!KeServiceDescriptorTableShadow[Index].Base) return FALSE;
    }

    /* Now clear from the Shadow Table. */
    KeServiceDescriptorTableShadow[Index].Base = NULL;
    KeServiceDescriptorTableShadow[Index].Number = NULL;
    KeServiceDescriptorTableShadow[Index].Limit = 0;
    KeServiceDescriptorTableShadow[Index].Count = NULL;

    /* Check if we should clean from the Master one too */
    if (Index == 1)
    {
        KeServiceDescriptorTable[Index].Base = NULL;
        KeServiceDescriptorTable[Index].Number = NULL;
        KeServiceDescriptorTable[Index].Limit = 0;
        KeServiceDescriptorTable[Index].Count = NULL;
    }

    return TRUE;
}
/* EOF */
