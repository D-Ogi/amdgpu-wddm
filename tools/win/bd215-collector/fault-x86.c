// Intentional faults of this test process only. Its SEH handler consumes them; no WER/UI.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <string.h>

static volatile DWORD jump_slots[32];
__declspec(naked) static void jump_return(void) {
    __asm {
        pop edi
        ret
    }
}
__declspec(naked) static void indirect_gp(void) {
    __asm {
        push edi
        lea eax, resume_here
        mov dword ptr [jump_slots + 8], eax
        mov edi, 2
        xor eax, eax
        mov ds, ax
        jmp dword ptr [edi * 4 + jump_slots]
    resume_here:
        pop edi
        ret
    }
}
__declspec(naked) static void far_gp(void) {
    __asm {
        _emit 0xea
        _emit 0
        _emit 0
        _emit 0
        _emit 0
        _emit 0
        _emit 0
    }
}
static int fault_filter(EXCEPTION_POINTERS* p) {
    // Exception dispatch executes with the normal runtime segments; preserve the actual stopped context in the collector.
    printf("selftest: handled code=0x%08lx address=%p info1=0x%08lx ds=0x%lx\n",
        p->ExceptionRecord->ExceptionCode, p->ExceptionRecord->ExceptionAddress,
        p->ExceptionRecord->NumberParameters > 1 ? (DWORD)p->ExceptionRecord->ExceptionInformation[1] : 0,
        p->ContextRecord->SegDs);
    return EXCEPTION_EXECUTE_HANDLER;
}
__declspec(noinline) static void ordinary_read(void) {
    volatile DWORD v = *(volatile DWORD*)(ULONG_PTR)0x1234;
    printf("unexpected %lu\n", v);
}
int main(int argc, char** argv) {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    const char* mode = argc > 1 ? argv[1] : "--gp";
    if (!strcmp(mode, "--clean")) return 0;
    if (!strcmp(mode, "--wait")) { Sleep(10000); return 0; }
    if (!strcmp(mode, "--delay-gp")) Sleep(1500);
    jump_slots[2] = (DWORD)(ULONG_PTR)jump_return;
    __try {
        if (!strcmp(mode, "--indirect-gp")) indirect_gp();
        else if (!strcmp(mode, "--read")) ordinary_read();
        else far_gp(); // Far JMP 0:0 has an invalid null code selector: architectural #GP.
    } __except (fault_filter(GetExceptionInformation())) {
        puts("selftest: handler-completed"); return 77;
    }
    puts("selftest: no exception"); return 78;
}
