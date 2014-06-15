/*
 * Preemptive kernel simulation under Windows
 * 
 * Eran Duchan, 2008 (released 2011)
 * www.pavius.net
 *
 */

#include <windows.h>
#include <string>
#include <queue>

// ============================================================================
// helpers
// ============================================================================

// crash/assert macro
#define wcs_assert(condition) do {if (!(condition)) wcs_crash(0);} while (0)
#define wcs_crash do {__asm int 3} while

// check alignment
#define wcs_is_aligned( address, alignment ) ( !(((unsigned int)address) & (alignment - 1)) )

// get offset of a structure member
#define wcs_offset_of(type, member) ( (unsigned int) &( (type *) 0 )->member )

// ============================================================================
// simulated ukernel
// ============================================================================

// thread control block
typedef struct
{
    // pointer to the stack - updated on context switch
    unsigned char *stack;

    // name, for debugging
    std::string name;

} WCS_THREAD_TCB;

// offset of stack pointer into TCB (can't use offset_of)
#define WCS_THREAD_TCB_STACK_PTR_OFFSET (0)

// thread entry with a single argument
typedef void (*WCS_THREAD_ENTRY)(void *);

// fifo queue of ready threads
std::queue < WCS_THREAD_TCB * > wcs_ready_threads;

// current running thread
WCS_THREAD_TCB *wcs_running_thread_tcb = NULL;

// called when a thread exits
void wcs_thread_entry_return()
{
    // normally, you would want to deallocate the TCB, remove the thread
    // from the kernel lists
}

// reschedule the next ready thread
void wcs_reschedule()
{
    //
    // Get next thread to run
    //

    // shove current thread to back of queue
    if (wcs_running_thread_tcb) wcs_ready_threads.push(wcs_running_thread_tcb);

    // get next available thread
    wcs_running_thread_tcb = wcs_ready_threads.front();
    wcs_ready_threads.pop();

    //
    // Load new thread context
    //    
    __asm
    {
        // load stack register from the new thread 
        mov eax, wcs_running_thread_tcb
        mov esp, WCS_THREAD_TCB_STACK_PTR_OFFSET[eax]

        // pop 8 GPRs 
        popad

        // pop the status word 
        popfd

        // stack now points to the entry. jump to it 
        ret
    }
}

// create a thread
void wcs_thread_create(const char *name, const unsigned int stack_size, 
                       const WCS_THREAD_ENTRY entry, void *argument)
{
    // check stack alignment
    wcs_assert(wcs_is_aligned(stack_size, sizeof(unsigned int)));

    // allocate a TCB
    WCS_THREAD_TCB *thread_tcb = new WCS_THREAD_TCB;
        
    // set thread name
    thread_tcb->name = name;

    // allocate the thread stack
    unsigned char *thread_stack = new unsigned char[stack_size];
    wcs_assert(thread_stack != NULL);

    // initialze it with some junk
    memset(thread_stack, 0xAB, stack_size);

    //
    // start shoving stuff into the stack
    //
    
    // set the thread stack in the TCB to the end of the stack block (since 
    // it grows upwards) and point to the first dword we can write to
    unsigned int *current_stack_position = (unsigned int *)((thread_stack + stack_size) - sizeof(unsigned int));

    // push argument 
    *current_stack_position-- = (unsigned int)argument;

    // this is the address of the routine to be called when a 
    // thread returns from its thread entry
    *current_stack_position-- = (unsigned int)wcs_thread_entry_return;

    // push entry 
    *current_stack_position-- = (unsigned int)entry;

    // push status word 
    *current_stack_position-- = 0x0202;

    // push 4 dummies - eax, ebx, ecx, edx. will be 
    // loaded into registers by popad
    
    *current_stack_position--    = 0xaa;        // eax 
    *current_stack_position--    = 0xcc;        // ecx 
    *current_stack_position--    = 0xdd;        // edx 
    *current_stack_position--    = 0xbb;        // ebx 
    *current_stack_position--    = 0x0;        // skipped (esp) 
    *current_stack_position--    = 0xeb;        // ebp 
    *current_stack_position--    = 0xa0;        // esi 
    *current_stack_position      = 0xb0;        // edi 

    // set current position in stack 
    thread_tcb->stack = (unsigned char *)current_stack_position;

    // shove thread to ready list
    wcs_ready_threads.push(thread_tcb);
}

// simulated tick ISR, called in main thread context via thread hijack 
__declspec(naked) void wcs_tick_isr()
{
    //
    // Save running thread context
    //
    __asm
    {
        // push status word 
        pushfd

        // push all registers  
        pushad

        // push stack ptr to running thread
        mov edx, wcs_running_thread_tcb
        mov [edx + WCS_THREAD_TCB_STACK_PTR_OFFSET], esp
    }

    // reschedule 
    wcs_reschedule();

    // in this simplistic demo, we'll never get here. in the real world,
    // we may get here if the kernel has no ready thread to execute. we'd 
    // need to restore the thread we interrupted and continue
    wcs_crash(0);
}

// ============================================================================
// Interrupt simulator thread
// ============================================================================

// interrupt simulator message types
typedef enum
{
    WCS_INTSIM_MSG_RAISE_INTERRUPT        = WM_USER + 1,

    // must be last
    WCS_INTSIM_MSG_LAST
    
} WCS_INTSIM_MESSAGE_TYPE;

// handles 
static HANDLE wcs_intsim_thread_handle, wcs_intsim_thread_to_interrupt_handle;
static DWORD wcs_intsim_thread_id;

// hijack the windows thread running our kernel - once
// it becomes ready it will jump to wcs_isr_reschedule
void wcs_intsim_do_interrupt()
{
    // initialize context flags 
    CONTEXT ctx;
        memset(&ctx, 0, sizeof(ctx));
        ctx.ContextFlags = CONTEXT_FULL;

    // suspend the kernel thread
    SuspendThread(wcs_intsim_thread_to_interrupt_handle);

    // get its windows thread context
    GetThreadContext(wcs_intsim_thread_to_interrupt_handle, &ctx);

    // push the address we want to return to (which is wherever the thread is now)
    // after our simulated ISR to the thread's stack
    ctx.Esp -= sizeof(unsigned int *);
    *(unsigned int *)ctx.Esp = ctx.Eip;

    // set the instruction pointer of the kernel thread to that of the ISR routine
    ctx.Eip = (DWORD)wcs_tick_isr;

    // set context of the kernel thread, effectively overriding the instruction pointer
    SetThreadContext(wcs_intsim_thread_to_interrupt_handle, &ctx);

    // resume the kernel thread
    ResumeThread(wcs_intsim_thread_to_interrupt_handle);
}

// raise interrupt 
void wcs_intsim_handle_raise_interrupt()
{
    // do the interrupt now 
    wcs_intsim_do_interrupt();
}

// thread entry 
DWORD WINAPI wcs_intsim_thread_entry(void *kernel_thread_handle)
{
    MSG thread_message;

    // save the handle of the thread we'll need to interrupt
    wcs_intsim_thread_to_interrupt_handle = (HANDLE)kernel_thread_handle;

    // forever 
    while (1)
    {
        // get windows message 
        if (GetMessage(&thread_message, NULL, WCS_INTSIM_MSG_RAISE_INTERRUPT, WCS_INTSIM_MSG_LAST) == TRUE)
        {
            // check which message we got 
            switch (thread_message.message)
            {
                // [async] raise interrupt 
                case WCS_INTSIM_MSG_RAISE_INTERRUPT: wcs_intsim_handle_raise_interrupt(); break;
            }
        }
        else
        {
            // error 
            wcs_crash(0);
        }
    }
}

// interrupt registered thread, called from periodic timer
void CALLBACK wcs_intsim_on_periodic_timer_expiration(void *param, BOOLEAN dummy)
{
    // send a message, don't wait 
    PostThreadMessage(wcs_intsim_thread_id, WCS_INTSIM_MSG_RAISE_INTERRUPT, 0, 0);
}

// start the periodic interrupt, simulating a round robin time-slicing
void wcs_intsim_start_periodic_interrupt()
{
    // timer handle (no need for it)
    HANDLE periodic_timer_handle;

    // create the timer, expiring periodically every 500 ms
    BOOL result = CreateTimerQueueTimer(&periodic_timer_handle, 
                                        CreateTimerQueue(), 
                                        (WAITORTIMERCALLBACK)wcs_intsim_on_periodic_timer_expiration, 
                                        NULL, 
                                        0, 
                                        500,
                                        0);

    /* make sure */
    wcs_assert(result == TRUE);
}

// initialize interrupt simulator 
void wcs_intsim_init(const HANDLE wcs_thread_to_interrupt_handle)
{
    // spawn interrupt thread 
    wcs_intsim_thread_handle = CreateThread(NULL, 0, wcs_intsim_thread_entry, 
                                            wcs_thread_to_interrupt_handle, 
                                            0, &wcs_intsim_thread_id);

    // make sure it's high priority so it can preempt us
    BOOL result = SetThreadPriority(wcs_intsim_thread_handle, THREAD_PRIORITY_HIGHEST);

    // wait a bit to allow thread to create messageq 
    Sleep(100);

    // start periodic timer
    wcs_intsim_start_periodic_interrupt();
}

// ============================================================================
// Demo
// ============================================================================

// incrementor thread
void thread_entry(void *argument)
{
    // increment a number
    while (1)
    {
        // print the argument repeatedly
        printf("%s\n", (const char *)argument);
    }
}

// get current thread handle
HANDLE get_main_thread_handle()
{
    HANDLE main_thread_handle = 0;

    // get it
    DuplicateHandle(GetCurrentProcess(),
                     GetCurrentThread(),
                     GetCurrentProcess(),
                     &main_thread_handle,
                     0,
                     TRUE,
                     DUPLICATE_SAME_ACCESS);

    // return it
    return main_thread_handle;
}

int main(int argc, char* argv[])
{
    // create threads
    wcs_thread_create("t1", 64 * 1024, thread_entry, (void *)" >> ");
    wcs_thread_create("t2", 64 * 1024, thread_entry, (void *)" >>>> ");
    wcs_thread_create("t3", 64 * 1024, thread_entry, (void *)" << ");
    wcs_thread_create("t4", 64 * 1024, thread_entry, (void *)" <<<< ");

    // initialize the interrupt simulator
    wcs_intsim_init(get_main_thread_handle());

    // start threads
    wcs_reschedule();

    // no error
    return 0;
}