//
//  Copyright © 2018 PubNative. All rights reserved.
//
//  Permission is hereby granted, free of charge, to any person obtaining a copy
//  of this software and associated documentation files (the "Software"), to deal
//  in the Software without restriction, including without limitation the rights
//  to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
//  copies of the Software, and to permit persons to whom the Software is
//  furnished to do so, subject to the following conditions:
//
//  The above copyright notice and this permission notice shall be included in
//  all copies or substantial portions of the Software.
//
//  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
//  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
//  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
//  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
//  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
//  OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
//  THE SOFTWARE.
//

#if defined(__i386__)

#include "PNLite_KSMach.h"

//#define PNLite_KSLogger_LocalLevel TRACE
#include "PNLite_KSLogger.h"

static const char *pnlite_g_registerNames[] = {
    "eax", "ebx",    "ecx", "edx", "edi", "esi", "ebp", "esp",
    "ss",  "eflags", "eip", "cs",  "ds",  "es",  "fs",  "gs",
};
static const int pnlite_g_registerNamesCount =
    sizeof(pnlite_g_registerNames) / sizeof(*pnlite_g_registerNames);

static const char *pnlite_g_exceptionRegisterNames[] = {"trapno", "err",
                                                     "faultvaddr"};
static const int pnlite_g_exceptionRegisterNamesCount =
    sizeof(pnlite_g_exceptionRegisterNames) /
    sizeof(*pnlite_g_exceptionRegisterNames);

uintptr_t
pnlite_ksmachframePointer(const PNLite_STRUCT_MCONTEXT_L *const machineContext) {
    return machineContext->__ss.__ebp;
}

uintptr_t
pnlite_ksmachstackPointer(const PNLite_STRUCT_MCONTEXT_L *const machineContext) {
    return machineContext->__ss.__esp;
}

uintptr_t pnlite_ksmachinstructionAddress(
    const PNLite_STRUCT_MCONTEXT_L *const machineContext) {
    return machineContext->__ss.__eip;
}

uintptr_t pnlite_ksmachlinkRegister(
    __unused const PNLite_STRUCT_MCONTEXT_L *const machineContext) {
    return 0;
}

bool pnlite_ksmachthreadState(const thread_t thread,
                           PNLite_STRUCT_MCONTEXT_L *const machineContext) {
    return pnlite_ksmachfillState(thread, (thread_state_t)&machineContext->__ss,
                               x86_THREAD_STATE32, x86_THREAD_STATE32_COUNT);
}

bool pnlite_ksmachfloatState(const thread_t thread,
                          PNLite_STRUCT_MCONTEXT_L *const machineContext) {
    return pnlite_ksmachfillState(thread, (thread_state_t)&machineContext->__fs,
                               x86_FLOAT_STATE32, x86_FLOAT_STATE32_COUNT);
}

bool pnlite_ksmachexceptionState(const thread_t thread,
                              PNLite_STRUCT_MCONTEXT_L *const machineContext) {
    return pnlite_ksmachfillState(thread, (thread_state_t)&machineContext->__es,
                               x86_EXCEPTION_STATE32,
                               x86_EXCEPTION_STATE32_COUNT);
}

int pnlite_ksmachnumRegisters(void) { return pnlite_g_registerNamesCount; }

const char *pnlite_ksmachregisterName(const int regNumber) {
    if (regNumber < pnlite_ksmachnumRegisters()) {
        return pnlite_g_registerNames[regNumber];
    }
    return NULL;
}

uint64_t
pnlite_ksmachregisterValue(const PNLite_STRUCT_MCONTEXT_L *const machineContext,
                        const int regNumber) {
    switch (regNumber) {
    case 0:
        return machineContext->__ss.__eax;
    case 1:
        return machineContext->__ss.__ebx;
    case 2:
        return machineContext->__ss.__ecx;
    case 3:
        return machineContext->__ss.__edx;
    case 4:
        return machineContext->__ss.__edi;
    case 5:
        return machineContext->__ss.__esi;
    case 6:
        return machineContext->__ss.__ebp;
    case 7:
        return machineContext->__ss.__esp;
    case 8:
        return machineContext->__ss.__ss;
    case 9:
        return machineContext->__ss.__eflags;
    case 10:
        return machineContext->__ss.__eip;
    case 11:
        return machineContext->__ss.__cs;
    case 12:
        return machineContext->__ss.__ds;
    case 13:
        return machineContext->__ss.__es;
    case 14:
        return machineContext->__ss.__fs;
    case 15:
        return machineContext->__ss.__gs;
    }

    PNLite_KSLOG_ERROR("Invalid register number: %d", regNumber);
    return 0;
}

int pnlite_ksmachnumExceptionRegisters(void) {
    return pnlite_g_exceptionRegisterNamesCount;
}

const char *pnlite_ksmachexceptionRegisterName(const int regNumber) {
    if (regNumber < pnlite_ksmachnumExceptionRegisters()) {
        return pnlite_g_exceptionRegisterNames[regNumber];
    }
    PNLite_KSLOG_ERROR("Invalid register number: %d", regNumber);
    return NULL;
}

uint64_t pnlite_ksmachexceptionRegisterValue(
    const PNLite_STRUCT_MCONTEXT_L *const machineContext, const int regNumber) {
    switch (regNumber) {
    case 0:
        return machineContext->__es.__trapno;
    case 1:
        return machineContext->__es.__err;
    case 2:
        return machineContext->__es.__faultvaddr;
    }

    PNLite_KSLOG_ERROR("Invalid register number: %d", regNumber);
    return 0;
}

uintptr_t
pnlite_ksmachfaultAddress(const PNLite_STRUCT_MCONTEXT_L *const machineContext) {
    return machineContext->__es.__faultvaddr;
}

int pnlite_ksmachstackGrowDirection(void) { return -1; }

#endif
