; Generic timing trampoline for hooked engine functions of unknown signature.
;
; Each hook gets a small stub (hooks.cpp) that does "mov r10, HookCtx*; mov r11, TdpxTimingThunk; jmp r11".
; r10/r11 are volatile and never carry arguments in the Windows x64 ABI, so every argument register arrives
; untouched. The thunk saves rcx/rdx/r8/r9 and xmm0-xmm3, calls TdpxThunkEnter, forwards the caller's first
; 16 stack arguments, calls the original function, keeps rax/xmm0, calls TdpxThunkLeave and returns.
;
; HookCtx layout: +0 original function pointer (MinHook trampoline). The rest is only read by C++.

EXTERN TdpxThunkEnter:PROC   ; uint64 TdpxThunkEnter(HookCtx* ctx, uint64* gprs, void* xmms, uint64* stackArgs)
EXTERN TdpxThunkLeave:PROC   ; void   TdpxThunkLeave(HookCtx* ctx, uint64 start)

FRAME_SIZE   EQU 1C8h        ; entry rsp is 8 mod 16; 1C8h keeps calls 16-byte aligned
OUT_ARGS     EQU 20h         ; outgoing stack arguments start after the 32-byte home area
NSTACK       EQU 16          ; caller stack arguments forwarded
SAVE_XMM     EQU 0B0h        ; xmm0..xmm3 at +0B0h, +0C0h, +0D0h, +0E0h
RET_XMM0     EQU 0F0h
SAVE_GPR     EQU 100h        ; rcx, rdx, r8, r9 at +100h..+118h
SAVE_CTX     EQU 120h
SAVE_START   EQU 128h
RET_RAX      EQU 130h
CALLER_ARGS  EQU FRAME_SIZE + 8 + 20h   ; caller's 5th argument: past our frame, return address, home area

.code

TdpxTimingThunk PROC FRAME
    sub     rsp, FRAME_SIZE
    .allocstack FRAME_SIZE
    .endprolog

    mov     [rsp+SAVE_GPR+00h], rcx
    mov     [rsp+SAVE_GPR+08h], rdx
    mov     [rsp+SAVE_GPR+10h], r8
    mov     [rsp+SAVE_GPR+18h], r9
    mov     [rsp+SAVE_CTX], r10
    movdqa  xmmword ptr [rsp+SAVE_XMM+00h], xmm0
    movdqa  xmmword ptr [rsp+SAVE_XMM+10h], xmm1
    movdqa  xmmword ptr [rsp+SAVE_XMM+20h], xmm2
    movdqa  xmmword ptr [rsp+SAVE_XMM+30h], xmm3

    mov     rcx, r10
    lea     rdx, [rsp+SAVE_GPR]
    lea     r8,  [rsp+SAVE_XMM]
    lea     r9,  [rsp+CALLER_ARGS]
    call    TdpxThunkEnter
    mov     [rsp+SAVE_START], rax

    xor     eax, eax
copy_args:
    mov     r11, [rsp+CALLER_ARGS+rax*8]
    mov     [rsp+OUT_ARGS+rax*8], r11
    inc     eax
    cmp     eax, NSTACK
    jb      copy_args

    mov     rcx, [rsp+SAVE_GPR+00h]
    mov     rdx, [rsp+SAVE_GPR+08h]
    mov     r8,  [rsp+SAVE_GPR+10h]
    mov     r9,  [rsp+SAVE_GPR+18h]
    movdqa  xmm0, xmmword ptr [rsp+SAVE_XMM+00h]
    movdqa  xmm1, xmmword ptr [rsp+SAVE_XMM+10h]
    movdqa  xmm2, xmmword ptr [rsp+SAVE_XMM+20h]
    movdqa  xmm3, xmmword ptr [rsp+SAVE_XMM+30h]
    mov     r10, [rsp+SAVE_CTX]
    call    qword ptr [r10]

    mov     [rsp+RET_RAX], rax
    movdqa  xmmword ptr [rsp+RET_XMM0], xmm0
    mov     rcx, [rsp+SAVE_CTX]
    mov     rdx, [rsp+SAVE_START]
    call    TdpxThunkLeave
    mov     rax, [rsp+RET_RAX]
    movdqa  xmm0, xmmword ptr [rsp+RET_XMM0]

    add     rsp, FRAME_SIZE
    ret
TdpxTimingThunk ENDP

END
