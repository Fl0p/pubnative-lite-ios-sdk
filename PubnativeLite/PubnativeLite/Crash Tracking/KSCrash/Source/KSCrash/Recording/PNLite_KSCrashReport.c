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

#include "PNLite_KSCrashReport.h"

#include "PNLite_KSBacktrace_Private.h"
#include "PNLite_KSCrashReportFields.h"
#include "PNLite_KSCrashReportVersion.h"
#include "PNLite_KSDynamicLinker.h"
#include "PNLite_KSFileUtils.h"
#include "PNLite_KSJSONCodec.h"
#include "PNLite_KSMach.h"
#include "PNLite_KSObjC.h"
#include "PNLite_KSSignalInfo.h"
#include "PNLite_KSString.h"
#include "PNLite_KSZombie.h"

//#define PNLite_kSLogger_LocalLevel TRACE
#include "PNLite_KSLogger.h"

#ifdef __arm64__
#include <sys/_types/_ucontext64.h>
#define PNLite_UC_MCONTEXT uc_mcontext64
typedef ucontext64_t SignalUserContext;
#else
#define PNLite_UC_MCONTEXT uc_mcontext
typedef ucontext_t SignalUserContext;
#endif

// Note: Avoiding static functions due to linker issues.

// ============================================================================
#pragma mark - Constants -
// ============================================================================

/** Maximum depth allowed for a backtrace. */
#define PNLite_kMaxBacktraceDepth 150

/** Default number of objects, subobjects, and ivars to record from a memory loc
 */
#define PNLite_kDefaultMemorySearchDepth 15

/** Length at which we consider a backtrace to represent a stack overflow.
 * If it reaches this point, we start cutting off from the top of the stack
 * rather than the bottom.
 */
#define PNLite_kStackOverflowThreshold 200

/** Maximum number of lines to print when printing a stack trace to the console.
 */
#define PNLite_kMaxStackTracePrintLines 40

/** How far to search the stack (in pointer sized jumps) for notable data. */
#define PNLite_kStackNotableSearchBackDistance 20
#define PNLite_kStackNotableSearchForwardDistance 10

/** How much of the stack to dump (in pointer sized jumps). */
#define PNLite_kStackContentsPushedDistance 20
#define PNLite_kStackContentsPoppedDistance 10
#define PNLite_kStackContentsTotalDistance                                        \
    (PNLite_kStackContentsPushedDistance + PNLite_kStackContentsPoppedDistance)

/** The minimum length for a valid string. */
#define PNLite_kMinStringLength 4

// ============================================================================
#pragma mark - Formatting -
// ============================================================================

#if defined(__LP64__)
#define PNLite_TRACE_FMT "%-4d%-31s 0x%016lx %s + %lu"
#define PNLite_POINTER_FMT "0x%016lx"
#define PNLite_POINTER_SHORT_FMT "0x%lx"
#else
#define PNLite_TRACE_FMT "%-4d%-31s 0x%08lx %s + %lu"
#define PNLite_POINTER_FMT "0x%08lx"
#define PNLite_POINTER_SHORT_FMT "0x%lx"
#endif

// ============================================================================
#pragma mark - JSON Encoding -
// ============================================================================

#define pnlite_getJsonContext(REPORT_WRITER)                                      \
    ((PNLite_KSJSONEncodeContext *)((REPORT_WRITER)->context))

/** Used for writing hex string values. */
static const char pnlite_g_hexNybbles[] = {'0', '1', '2', '3', '4', '5', '6', '7',
                                        '8', '9', 'A', 'B', 'C', 'D', 'E', 'F'};

// ============================================================================
#pragma mark - Runtime Config -
// ============================================================================

static PNLite_KSCrash_IntrospectionRules *pnlite_g_introspectionRules;

#pragma mark Callbacks

void pnlite_kscrw_i_addBooleanElement(const PNLite_KSCrashReportWriter *const writer,
                                   const char *const key, const bool value) {
    pnlite_ksjsonaddBooleanElement(pnlite_getJsonContext(writer), key, value);
}

void pnlite_kscrw_i_addFloatingPointElement(
    const PNLite_KSCrashReportWriter *const writer, const char *const key,
    const double value) {
    pnlite_ksjsonaddFloatingPointElement(pnlite_getJsonContext(writer), key, value);
}

void pnlite_kscrw_i_addIntegerElement(const PNLite_KSCrashReportWriter *const writer,
                                   const char *const key,
                                   const long long value) {
    pnlite_ksjsonaddIntegerElement(pnlite_getJsonContext(writer), key, value);
}

void pnlite_kscrw_i_addUIntegerElement(const PNLite_KSCrashReportWriter *const writer,
                                    const char *const key,
                                    const unsigned long long value) {
    pnlite_ksjsonaddIntegerElement(pnlite_getJsonContext(writer), key,
                                (long long)value);
}

void pnlite_kscrw_i_addStringElement(const PNLite_KSCrashReportWriter *const writer,
                                  const char *const key,
                                  const char *const value) {
    pnlite_ksjsonaddStringElement(pnlite_getJsonContext(writer), key, value,
                               PNLite_KSJSON_SIZE_AUTOMATIC);
}

void pnlite_kscrw_i_addTextFileElement(const PNLite_KSCrashReportWriter *const writer,
                                    const char *const key,
                                    const char *const filePath) {
    const int fd = open(filePath, O_RDONLY);
    if (fd < 0) {
        PNLite_KSLOG_ERROR("Could not open file %s: %s", filePath,
                        strerror(errno));
        return;
    }

    if (pnlite_ksjsonbeginStringElement(pnlite_getJsonContext(writer), key) !=
        PNLite_KSJSON_OK) {
        PNLite_KSLOG_ERROR("Could not start string element");
        goto done;
    }

    char buffer[512];
    ssize_t bytesRead;
    for (bytesRead = read(fd, buffer, sizeof(buffer)); bytesRead > 0;
         bytesRead = read(fd, buffer, sizeof(buffer))) {
        if (pnlite_ksjsonappendStringElement(pnlite_getJsonContext(writer), buffer,
                                          (size_t)bytesRead) != PNLite_KSJSON_OK) {
            PNLite_KSLOG_ERROR("Could not append string element");
            goto done;
        }
    }

done:
    pnlite_ksjsonendStringElement(pnlite_getJsonContext(writer));
    close(fd);
}

void pnlite_kscrw_i_addDataElement(const PNLite_KSCrashReportWriter *const writer,
                                const char *const key, const char *const value,
                                const size_t length) {
    pnlite_ksjsonaddDataElement(pnlite_getJsonContext(writer), key, value, length);
}

void pnlite_kscrw_i_beginDataElement(const PNLite_KSCrashReportWriter *const writer,
                                  const char *const key) {
    pnlite_ksjsonbeginDataElement(pnlite_getJsonContext(writer), key);
}

void pnlite_kscrw_i_appendDataElement(const PNLite_KSCrashReportWriter *const writer,
                                   const char *const value,
                                   const size_t length) {
    pnlite_ksjsonappendDataElement(pnlite_getJsonContext(writer), value, length);
}

void pnlite_kscrw_i_endDataElement(const PNLite_KSCrashReportWriter *const writer) {
    pnlite_ksjsonendDataElement(pnlite_getJsonContext(writer));
}

void pnlite_kscrw_i_addUUIDElement(const PNLite_KSCrashReportWriter *const writer,
                                const char *const key,
                                const unsigned char *const value) {
    if (value == NULL) {
        pnlite_ksjsonaddNullElement(pnlite_getJsonContext(writer), key);
    } else {
        char uuidBuffer[37];
        const unsigned char *src = value;
        char *dst = uuidBuffer;
        for (int i = 0; i < 4; i++) {
            *dst++ = pnlite_g_hexNybbles[(*src >> 4) & 15];
            *dst++ = pnlite_g_hexNybbles[(*src++) & 15];
        }
        *dst++ = '-';
        for (int i = 0; i < 2; i++) {
            *dst++ = pnlite_g_hexNybbles[(*src >> 4) & 15];
            *dst++ = pnlite_g_hexNybbles[(*src++) & 15];
        }
        *dst++ = '-';
        for (int i = 0; i < 2; i++) {
            *dst++ = pnlite_g_hexNybbles[(*src >> 4) & 15];
            *dst++ = pnlite_g_hexNybbles[(*src++) & 15];
        }
        *dst++ = '-';
        for (int i = 0; i < 2; i++) {
            *dst++ = pnlite_g_hexNybbles[(*src >> 4) & 15];
            *dst++ = pnlite_g_hexNybbles[(*src++) & 15];
        }
        *dst++ = '-';
        for (int i = 0; i < 6; i++) {
            *dst++ = pnlite_g_hexNybbles[(*src >> 4) & 15];
            *dst++ = pnlite_g_hexNybbles[(*src++) & 15];
        }

        pnlite_ksjsonaddStringElement(pnlite_getJsonContext(writer), key, uuidBuffer,
                                   (size_t)(dst - uuidBuffer));
    }
}

void pnlite_kscrw_i_addJSONElement(const PNLite_KSCrashReportWriter *const writer,
                                const char *const key,
                                const char *const jsonElement) {
    int jsonResult = pnlite_ksjsonaddJSONElement(pnlite_getJsonContext(writer), key,
                                              jsonElement, strlen(jsonElement));
    if (jsonResult != PNLite_KSJSON_OK) {
        char errorBuff[100];
        snprintf(errorBuff, sizeof(errorBuff), "Invalid JSON data: %s",
                 pnlite_ksjsonstringForError(jsonResult));
        pnlite_ksjsonbeginObject(pnlite_getJsonContext(writer), key);
        pnlite_ksjsonaddStringElement(pnlite_getJsonContext(writer),
                                   PNLite_KSCrashField_Error, errorBuff,
                                   PNLite_KSJSON_SIZE_AUTOMATIC);
        pnlite_ksjsonaddStringElement(pnlite_getJsonContext(writer),
                                   PNLite_KSCrashField_JSONData, jsonElement,
                                   PNLite_KSJSON_SIZE_AUTOMATIC);
        pnlite_ksjsonendContainer(pnlite_getJsonContext(writer));
    }
}

void pnlite_kscrw_i_addJSONElementFromFile(
    const PNLite_KSCrashReportWriter *const writer, const char *const key,
    const char *const filePath) {
    const int fd = open(filePath, O_RDONLY);
    if (fd < 0) {
        PNLite_KSLOG_ERROR("Could not open file %s: %s", filePath,
                        strerror(errno));
        return;
    }

    if (pnlite_ksjsonbeginElement(pnlite_getJsonContext(writer), key) !=
        PNLite_KSJSON_OK) {
        PNLite_KSLOG_ERROR("Could not start JSON element");
        goto done;
    }

    char buffer[512];
    ssize_t bytesRead;
    while ((bytesRead = read(fd, buffer, sizeof(buffer))) > 0) {
        if (pnlite_ksjsonaddRawJSONData(pnlite_getJsonContext(writer), buffer,
                                     (size_t)bytesRead) != PNLite_KSJSON_OK) {
            PNLite_KSLOG_ERROR("Could not append JSON data");
            goto done;
        }
    }

done:
    close(fd);
}

void pnlite_kscrw_i_beginObject(const PNLite_KSCrashReportWriter *const writer,
                             const char *const key) {
    pnlite_ksjsonbeginObject(pnlite_getJsonContext(writer), key);
}

void pnlite_kscrw_i_beginArray(const PNLite_KSCrashReportWriter *const writer,
                            const char *const key) {
    pnlite_ksjsonbeginArray(pnlite_getJsonContext(writer), key);
}

void pnlite_kscrw_i_endContainer(const PNLite_KSCrashReportWriter *const writer) {
    pnlite_ksjsonendContainer(pnlite_getJsonContext(writer));
}

int pnlite_kscrw_i_addJSONData(const char *const data, const size_t length,
                            void *const userData) {
    const int fd = *((int *)userData);
    const bool success = pnlite_ksfuwriteBytesToFD(fd, data, (ssize_t)length);
    return success ? PNLite_KSJSON_OK : PNLite_KSJSON_ERROR_CANNOT_ADD_DATA;
}

// ============================================================================
#pragma mark - Utility -
// ============================================================================

/** Check if a memory address points to a valid null terminated UTF-8 string.
 *
 * @param address The address to check.
 *
 * @return true if the address points to a string.
 */
bool pnlite_kscrw_i_isValidString(const void *const address) {
    if ((void *)address == NULL) {
        return false;
    }

    char buffer[500];
    if ((uintptr_t)address + sizeof(buffer) < (uintptr_t)address) {
        // Wrapped around the address range.
        return false;
    }
    if (pnlite_ksmachcopyMem(address, buffer, sizeof(buffer)) != KERN_SUCCESS) {
        return false;
    }
    return pnlite_ksstring_isNullTerminatedUTF8String(buffer, PNLite_kMinStringLength,
                                                   sizeof(buffer));
}

/** Get all parts of the machine state required for a dump.
 * This includes basic thread state, and exception registers.
 *
 * @param thread The thread to get state for.
 *
 * @param machineContextBuffer The machine context to fill out.
 */
bool pnlite_kscrw_i_fetchMachineState(
    const thread_t thread, PNLite_STRUCT_MCONTEXT_L *const machineContextBuffer) {
    if (!pnlite_ksmachthreadState(thread, machineContextBuffer)) {
        return false;
    }

    if (!pnlite_ksmachexceptionState(thread, machineContextBuffer)) {
        return false;
    }

    return true;
}

/** Get the machine context for the specified thread.
 *
 * This function will choose how to fetch the machine context based on what kind
 * of thread it is (current, crashed, other), and what kind of crash occured.
 * It may store the context in machineContextBuffer unless it can be fetched
 * directly from memory. Do not count on machineContextBuffer containing
 * anything. Always use the return value.
 *
 * @param crash The crash handler context.
 *
 * @param thread The thread to get a machine context for.
 *
 * @param machineContextBuffer A place to store the context, if needed.
 *
 * @return A pointer to the crash context, or NULL if not found.
 */
PNLite_STRUCT_MCONTEXT_L *pnlite_kscrw_i_getMachineContext(
    const PNLite_KSCrash_SentryContext *const crash, const thread_t thread,
    PNLite_STRUCT_MCONTEXT_L *const machineContextBuffer) {
    if (thread == crash->offendingThread) {
        if (crash->crashType == PNLite_KSCrashTypeSignal) {
            return ((SignalUserContext *)crash->signal.userContext)
                ->PNLite_UC_MCONTEXT;
        }
    }

    if (thread == pnlite_ksmachthread_self()) {
        return NULL;
    }

    if (!pnlite_kscrw_i_fetchMachineState(thread, machineContextBuffer)) {
        PNLite_KSLOG_ERROR("Failed to fetch machine state for thread %d", thread);
        return NULL;
    }

    return machineContextBuffer;
}

/** Get the backtrace for the specified thread.
 *
 * This function will choose how to fetch the backtrace based on machine context
 * availability andwhat kind of crash occurred. It may store the backtrace in
 * backtraceBuffer unless it can be fetched directly from memory. Do not count
 * on backtraceBuffer containing anything. Always use the return value.
 *
 * @param crash The crash handler context.
 *
 * @param thread The thread to get a machine context for.
 *
 * @param machineContext The machine context (can be NULL).
 *
 * @param backtraceBuffer A place to store the backtrace, if needed.
 *
 * @param backtraceLength In: The length of backtraceBuffer.
 *                        Out: The length of the backtrace.
 *
 * @param skippedEntries Out: The number of entries that were skipped due to
 *                             stack overflow.
 *
 * @return The backtrace, or NULL if not found.
 */
uintptr_t *pnlite_kscrw_i_getBacktrace(
    const PNLite_KSCrash_SentryContext *const crash, const thread_t thread,
    const PNLite_STRUCT_MCONTEXT_L *const machineContext,
    uintptr_t *const backtraceBuffer, int *const backtraceLength,
    int *const skippedEntries) {
    if (thread == crash->offendingThread) {
        if (crash->stackTrace != NULL && crash->stackTraceLength > 0 &&
            (crash->crashType &
             (PNLite_KSCrashTypeCPPException | PNLite_KSCrashTypeNSException |
              PNLite_KSCrashTypeUserReported))) {
            *backtraceLength = crash->stackTraceLength;
            return crash->stackTrace;
        }
    }

    if (machineContext == NULL) {
        return NULL;
    }

    int actualSkippedEntries = 0;
    int actualLength = pnlite_ksbt_backtraceLength(machineContext);
    if (actualLength >= PNLite_kStackOverflowThreshold) {
        actualSkippedEntries = actualLength - *backtraceLength;
    }

    *backtraceLength =
        pnlite_ksbt_backtraceThreadState(machineContext, backtraceBuffer,
                                      actualSkippedEntries, *backtraceLength);
    if (skippedEntries != NULL) {
        *skippedEntries = actualSkippedEntries;
    }
    return backtraceBuffer;
}

/** Check if the stack for the specified thread has overflowed.
 *
 * @param crash The crash handler context.
 *
 * @param thread The thread to check.
 *
 * @return true if the thread's stack has overflowed.
 */
bool pnlite_kscrw_i_isStackOverflow(const PNLite_KSCrash_SentryContext *const crash,
                                 const thread_t thread) {
    PNLite_STRUCT_MCONTEXT_L concreteMachineContext;
    PNLite_STRUCT_MCONTEXT_L *machineContext =
        pnlite_kscrw_i_getMachineContext(crash, thread, &concreteMachineContext);
    if (machineContext == NULL) {
        return false;
    }

    return pnlite_ksbt_isBacktraceTooLong(machineContext,
                                       PNLite_kStackOverflowThreshold);
}

// ============================================================================
#pragma mark - Console Logging -
// ============================================================================

/** Print the crash type and location to the log.
 *
 * @param sentryContext The crash sentry context.
 */
void pnlite_kscrw_i_logCrashType(
    const PNLite_KSCrash_SentryContext *const sentryContext) {
    switch (sentryContext->crashType) {
    case PNLite_KSCrashTypeMachException: {
        int machExceptionType = sentryContext->mach.type;
        kern_return_t machCode = (kern_return_t)sentryContext->mach.code;
        const char *machExceptionName =
            pnlite_ksmachexceptionName(machExceptionType);
        const char *machCodeName =
            machCode == 0 ? NULL : pnlite_ksmachkernelReturnCodeName(machCode);
        PNLite_KSLOGBASIC_INFO("App crashed due to mach exception: [%s: %s] at %p",
                            machExceptionName, machCodeName,
                            sentryContext->faultAddress);
        break;
    }
    case PNLite_KSCrashTypeCPPException: {
        PNLite_KSLOG_INFO("App crashed due to C++ exception: %s: %s",
                       sentryContext->CPPException.name,
                       sentryContext->crashReason);
        break;
    }
    case PNLite_KSCrashTypeNSException: {
        PNLite_KSLOGBASIC_INFO("App crashed due to NSException: %s: %s",
                            sentryContext->NSException.name,
                            sentryContext->crashReason);
        break;
    }
    case PNLite_KSCrashTypeSignal: {
        int sigNum = sentryContext->signal.signalInfo->si_signo;
        int sigCode = sentryContext->signal.signalInfo->si_code;
        const char *sigName = pnlite_kssignal_signalName(sigNum);
        const char *sigCodeName = pnlite_kssignal_signalCodeName(sigNum, sigCode);
        PNLite_KSLOGBASIC_INFO("App crashed due to signal: [%s, %s] at %08x",
                            sigName, sigCodeName, sentryContext->faultAddress);
        break;
    }
    case PNLite_KSCrashTypeMainThreadDeadlock: {
        PNLite_KSLOGBASIC_INFO("Main thread deadlocked");
        break;
    }
    case PNLite_KSCrashTypeUserReported: {
        PNLite_KSLOG_INFO("App crashed due to user specified exception: %s",
                       sentryContext->crashReason);
        break;
    }
    }
}

/** Print a backtrace entry in the standard format to the log.
 *
 * @param entryNum The backtrace entry number.
 *
 * @param address The program counter value (instruction address).
 *
 * @param dlInfo Information about the nearest symbols to the address.
 */
void pnlite_kscrw_i_logBacktraceEntry(const int entryNum, const uintptr_t address,
                                   const Dl_info *const dlInfo) {
    char faddrBuff[20];
    char saddrBuff[20];

    const char *fname = pnlite_ksfulastPathEntry(dlInfo->dli_fname);
    if (fname == NULL) {
        sprintf(faddrBuff, PNLite_POINTER_FMT, (uintptr_t)dlInfo->dli_fbase);
        fname = faddrBuff;
    }

    uintptr_t offset = address - (uintptr_t)dlInfo->dli_saddr;
    const char *sname = dlInfo->dli_sname;
    if (sname == NULL) {
        sprintf(saddrBuff, PNLite_POINTER_SHORT_FMT, (uintptr_t)dlInfo->dli_fbase);
        sname = saddrBuff;
        offset = address - (uintptr_t)dlInfo->dli_fbase;
    }

    PNLite_KSLOGBASIC_ALWAYS(PNLite_TRACE_FMT, entryNum, fname, address, sname,
                          offset);
}

/** Print a backtrace to the log.
 *
 * @param backtrace The backtrace to print.
 *
 * @param backtraceLength The length of the backtrace.
 */
void pnlite_kscrw_i_logBacktrace(const uintptr_t *const backtrace,
                              const int backtraceLength,
                              const int skippedEntries) {
    if (backtraceLength > 0) {
        Dl_info symbolicated[backtraceLength];
        pnlite_ksbt_symbolicate(backtrace, symbolicated, backtraceLength,
                             skippedEntries);

        for (int i = 0; i < backtraceLength; i++) {
            pnlite_kscrw_i_logBacktraceEntry(i, backtrace[i], &symbolicated[i]);
        }
    }
}

/** Print the backtrace for the crashed thread to the log.
 *
 * @param crash The crash handler context.
 */
void pnlite_kscrw_i_logCrashThreadBacktrace(
    const PNLite_KSCrash_SentryContext *const crash) {
    thread_t thread = crash->offendingThread;
    PNLite_STRUCT_MCONTEXT_L concreteMachineContext;
    uintptr_t concreteBacktrace[PNLite_kMaxStackTracePrintLines];
    int backtraceLength =
        sizeof(concreteBacktrace) / sizeof(*concreteBacktrace);

    PNLite_STRUCT_MCONTEXT_L *machineContext =
        pnlite_kscrw_i_getMachineContext(crash, thread, &concreteMachineContext);

    int skippedEntries = 0;
    uintptr_t *backtrace = pnlite_kscrw_i_getBacktrace(
        crash, thread, machineContext, concreteBacktrace, &backtraceLength,
        &skippedEntries);

    if (backtrace != NULL) {
        pnlite_kscrw_i_logBacktrace(backtrace, backtraceLength, skippedEntries);
    }
}

// ============================================================================
#pragma mark - Report Writing -
// ============================================================================

/** Write the contents of a memory location.
 * Also writes meta information about the data.
 *
 * @param writer The writer.
 *
 * @param key The object key, if needed.
 *
 * @param address The memory address.
 *
 * @param limit How many more subreferenced objects to write, if any.
 */
void pnlite_kscrw_i_writeMemoryContents(
    const PNLite_KSCrashReportWriter *const writer, const char *const key,
    const uintptr_t address, int *limit);

/** Write a string to the report.
 * This will only print the first child of the array.
 *
 * @param writer The writer.
 *
 * @param key The object key, if needed.
 *
 * @param objectAddress The object's address.
 *
 * @param limit How many more subreferenced objects to write, if any.
 */
void pnlite_kscrw_i_writeNSStringContents(
    const PNLite_KSCrashReportWriter *const writer, const char *const key,
    const uintptr_t objectAddress, __unused int *limit) {
    const void *object = (const void *)objectAddress;
    char buffer[200];
    if (pnlite_ksobjc_copyStringContents(object, buffer, sizeof(buffer))) {
        writer->addStringElement(writer, key, buffer);
    }
}

/** Write a URL to the report.
 * This will only print the first child of the array.
 *
 * @param writer The writer.
 *
 * @param key The object key, if needed.
 *
 * @param objectAddress The object's address.
 *
 * @param limit How many more subreferenced objects to write, if any.
 */
void pnlite_kscrw_i_writeURLContents(const PNLite_KSCrashReportWriter *const writer,
                                  const char *const key,
                                  const uintptr_t objectAddress,
                                  __unused int *limit) {
    const void *object = (const void *)objectAddress;
    char buffer[200];
    if (pnlite_ksobjc_copyStringContents(object, buffer, sizeof(buffer))) {
        writer->addStringElement(writer, key, buffer);
    }
}

/** Write a date to the report.
 * This will only print the first child of the array.
 *
 * @param writer The writer.
 *
 * @param key The object key, if needed.
 *
 * @param objectAddress The object's address.
 *
 * @param limit How many more subreferenced objects to write, if any.
 */
void pnlite_kscrw_i_writeDateContents(const PNLite_KSCrashReportWriter *const writer,
                                   const char *const key,
                                   const uintptr_t objectAddress,
                                   __unused int *limit) {
    const void *object = (const void *)objectAddress;
    writer->addFloatingPointElement(writer, key,
                                    pnlite_ksobjc_dateContents(object));
}

/** Write a number to the report.
 * This will only print the first child of the array.
 *
 * @param writer The writer.
 *
 * @param key The object key, if needed.
 *
 * @param objectAddress The object's address.
 *
 * @param limit How many more subreferenced objects to write, if any.
 */
void pnlite_kscrw_i_writeNumberContents(
    const PNLite_KSCrashReportWriter *const writer, const char *const key,
    const uintptr_t objectAddress, __unused int *limit) {
    const void *object = (const void *)objectAddress;
    writer->addFloatingPointElement(writer, key,
                                    pnlite_ksobjc_numberAsFloat(object));
}

/** Write an array to the report.
 * This will only print the first child of the array.
 *
 * @param writer The writer.
 *
 * @param key The object key, if needed.
 *
 * @param objectAddress The object's address.
 *
 * @param limit How many more subreferenced objects to write, if any.
 */
void pnlite_kscrw_i_writeArrayContents(const PNLite_KSCrashReportWriter *const writer,
                                    const char *const key,
                                    const uintptr_t objectAddress, int *limit) {
    const void *object = (const void *)objectAddress;
    uintptr_t firstObject;
    if (pnlite_ksobjc_arrayContents(object, &firstObject, 1) == 1) {
        pnlite_kscrw_i_writeMemoryContents(writer, key, firstObject, limit);
    }
}

/** Write out ivar information about an unknown object.
 *
 * @param writer The writer.
 *
 * @param key The object key, if needed.
 *
 * @param objectAddress The object's address.
 *
 * @param limit How many more subreferenced objects to write, if any.
 */
void pnlite_kscrw_i_writeUnknownObjectContents(
    const PNLite_KSCrashReportWriter *const writer, const char *const key,
    const uintptr_t objectAddress, int *limit) {
    (*limit)--;
    const void *object = (const void *)objectAddress;
    PNLite_KSObjCIvar ivars[10];
    char s8;
    short s16;
    int sInt;
    long s32;
    long long s64;
    unsigned char u8;
    unsigned short u16;
    unsigned int uInt;
    unsigned long u32;
    unsigned long long u64;
    float f32;
    double f64;
    _Bool b;
    void *pointer;

    writer->beginObject(writer, key);
    {
        if (pnlite_ksobjc_pnlite_isTaggedPointer(object)) {
            writer->addIntegerElement(
                writer, "tagged_payload",
                (long long)pnlite_ksobjc_taggedPointerPayload(object));
        } else {
            const void *class = pnlite_ksobjc_isaPointer(object);
            size_t ivarCount = pnlite_ksobjc_ivarList(
                class, ivars, sizeof(ivars) / sizeof(*ivars));
            *limit -= (int)ivarCount;
            for (size_t i = 0; i < ivarCount; i++) {
                PNLite_KSObjCIvar *ivar = &ivars[i];
                
                if (ivar->type == NULL) {
                    PNLite_KSLOG_ERROR("Found null ivar :(");
                    continue;
                }
                
                switch (ivar->type[0]) {
                case 'c':
                    pnlite_ksobjc_ivarValue(object, ivar->index, &s8);
                    writer->addIntegerElement(writer, ivar->name, s8);
                    break;
                case 'i':
                    pnlite_ksobjc_ivarValue(object, ivar->index, &sInt);
                    writer->addIntegerElement(writer, ivar->name, sInt);
                    break;
                case 's':
                    pnlite_ksobjc_ivarValue(object, ivar->index, &s16);
                    writer->addIntegerElement(writer, ivar->name, s16);
                    break;
                case 'l':
                    pnlite_ksobjc_ivarValue(object, ivar->index, &s32);
                    writer->addIntegerElement(writer, ivar->name, s32);
                    break;
                case 'q':
                    pnlite_ksobjc_ivarValue(object, ivar->index, &s64);
                    writer->addIntegerElement(writer, ivar->name, s64);
                    break;
                case 'C':
                    pnlite_ksobjc_ivarValue(object, ivar->index, &u8);
                    writer->addUIntegerElement(writer, ivar->name, u8);
                    break;
                case 'I':
                    pnlite_ksobjc_ivarValue(object, ivar->index, &uInt);
                    writer->addUIntegerElement(writer, ivar->name, uInt);
                    break;
                case 'S':
                    pnlite_ksobjc_ivarValue(object, ivar->index, &u16);
                    writer->addUIntegerElement(writer, ivar->name, u16);
                    break;
                case 'L':
                    pnlite_ksobjc_ivarValue(object, ivar->index, &u32);
                    writer->addUIntegerElement(writer, ivar->name, u32);
                    break;
                case 'Q':
                    pnlite_ksobjc_ivarValue(object, ivar->index, &u64);
                    writer->addUIntegerElement(writer, ivar->name, u64);
                    break;
                case 'f':
                    pnlite_ksobjc_ivarValue(object, ivar->index, &f32);
                    writer->addFloatingPointElement(writer, ivar->name, f32);
                    break;
                case 'd':
                    pnlite_ksobjc_ivarValue(object, ivar->index, &f64);
                    writer->addFloatingPointElement(writer, ivar->name, f64);
                    break;
                case 'B':
                    pnlite_ksobjc_ivarValue(object, ivar->index, &b);
                    writer->addBooleanElement(writer, ivar->name, b);
                    break;
                case '*':
                case '@':
                case '#':
                case ':':
                    pnlite_ksobjc_ivarValue(object, ivar->index, &pointer);
                    pnlite_kscrw_i_writeMemoryContents(writer, ivar->name,
                                                    (uintptr_t)pointer, limit);
                    break;
                default:
                    PNLite_KSLOG_DEBUG("%s: Unknown ivar type [%s]", ivar->name,
                                    ivar->type);
                }
            }
        }
    }
    writer->endContainer(writer);
}

bool pnlite_kscrw_i_isRestrictedClass(const char *name) {
    if (pnlite_g_introspectionRules->restrictedClasses != NULL) {
        for (size_t i = 0; i < pnlite_g_introspectionRules->restrictedClassesCount;
             i++) {
            if (strcmp(name, pnlite_g_introspectionRules->restrictedClasses[i]) ==
                0) {
                return true;
            }
        }
    }
    return false;
}

/** Write the contents of a memory location.
 * Also writes meta information about the data.
 *
 * @param writer The writer.
 *
 * @param key The object key, if needed.
 *
 * @param address The memory address.
 *
 * @param limit How many more subreferenced objects to write, if any.
 */
void pnlite_kscrw_i_writeMemoryContents(
    const PNLite_KSCrashReportWriter *const writer, const char *const key,
    const uintptr_t address, int *limit) {
    (*limit)--;
    const void *object = (const void *)address;
    writer->beginObject(writer, key);
    {
        writer->addUIntegerElement(writer, PNLite_KSCrashField_Address, address);
        const char *zombieClassName = pnlite_kszombie_className(object);
        if (zombieClassName != NULL) {
            writer->addStringElement(writer, PNLite_KSCrashField_LastDeallocObject,
                                     zombieClassName);
        }
        switch (pnlite_ksobjc_objectType(object)) {
            case PNLite_KSObjCTypeUnknown:
            if (object == NULL) {
                writer->addStringElement(writer, PNLite_KSCrashField_Type,
                                         PNLite_KSCrashMemType_NullPointer);
            } else if (pnlite_kscrw_i_isValidString(object)) {
                writer->addStringElement(writer, PNLite_KSCrashField_Type,
                                         PNLite_KSCrashMemType_String);
                writer->addStringElement(writer, PNLite_KSCrashField_Value,
                                         (const char *)object);
            } else {
                writer->addStringElement(writer, PNLite_KSCrashField_Type,
                                         PNLite_KSCrashMemType_Unknown);
            }
            break;
        case PNLite_KSObjCTypeClass:
            writer->addStringElement(writer, PNLite_KSCrashField_Type,
                                     PNLite_KSCrashMemType_Class);
            writer->addStringElement(writer, PNLite_KSCrashField_Class,
                                     pnlite_ksobjc_className(object));
            break;
        case PNLite_KSObjCTypeObject: {
            writer->addStringElement(writer, PNLite_KSCrashField_Type,
                                     PNLite_KSCrashMemType_Object);
            const char *className = pnlite_ksobjc_objectClassName(object);
            writer->addStringElement(writer, PNLite_KSCrashField_Class, className);
            if (!pnlite_kscrw_i_isRestrictedClass(className)) {
                switch (pnlite_ksobjc_objectClassType(object)) {
                case PNLite_KSObjCClassTypeString:
                    pnlite_kscrw_i_writeNSStringContents(
                        writer, PNLite_KSCrashField_Value, address, limit);
                    break;
                case PNLite_KSObjCClassTypeURL:
                    pnlite_kscrw_i_writeURLContents(writer, PNLite_KSCrashField_Value,
                                                 address, limit);
                    break;
                case PNLite_KSObjCClassTypeDate:
                    pnlite_kscrw_i_writeDateContents(
                        writer, PNLite_KSCrashField_Value, address, limit);
                    break;
                case PNLite_KSObjCClassTypeArray:
                    if (*limit > 0) {
                        pnlite_kscrw_i_writeArrayContents(
                            writer, PNLite_KSCrashField_FirstObject, address,
                            limit);
                    }
                    break;
                case PNLite_KSObjCClassTypeNumber:
                    pnlite_kscrw_i_writeNumberContents(
                        writer, PNLite_KSCrashField_Value, address, limit);
                    break;
                case PNLite_KSObjCClassTypeDictionary:
                case PNLite_KSObjCClassTypeException:
                    // TODO: Implement these.
                    if (*limit > 0) {
                        pnlite_kscrw_i_writeUnknownObjectContents(
                            writer, PNLite_KSCrashField_Ivars, address, limit);
                    }
                    break;
                case PNLite_KSObjCClassTypeUnknown:
                    if (*limit > 0) {
                        pnlite_kscrw_i_writeUnknownObjectContents(
                            writer, PNLite_KSCrashField_Ivars, address, limit);
                    }
                    break;
                }
            }
            break;
        }
        case PNLite_KSObjCTypeBlock:
            writer->addStringElement(writer, PNLite_KSCrashField_Type,
                                     PNLite_KSCrashMemType_Block);
            const char *className = pnlite_ksobjc_objectClassName(object);
            writer->addStringElement(writer, PNLite_KSCrashField_Class, className);
            break;
        }
    }
    writer->endContainer(writer);
}

bool pnlite_kscrw_i_isValidPointer(const uintptr_t address) {
    if (address == (uintptr_t)NULL) {
        return false;
    }

    if (pnlite_ksobjc_pnlite_isTaggedPointer((const void *)address)) {
        if (!pnlite_ksobjc_isValidTaggedPointer((const void *)address)) {
            return false;
        }
    }

    return true;
}

/** Write the contents of a memory location only if it contains notable data.
 * Also writes meta information about the data.
 *
 * @param writer The writer.
 *
 * @param key The object key, if needed.
 *
 * @param address The memory address.
 */
void pnlite_kscrw_i_writeMemoryContentsIfNotable(
    const PNLite_KSCrashReportWriter *const writer, const char *const key,
    const uintptr_t address) {
    if (!pnlite_kscrw_i_isValidPointer(address)) {
        return;
    }

    const void *object = (const void *)address;

    if (pnlite_ksobjc_objectType(object) == PNLite_KSObjCTypeUnknown &&
        pnlite_kszombie_className(object) == NULL &&
        !pnlite_kscrw_i_isValidString(object)) {
        // Nothing notable about this memory location.
        return;
    }

    int limit = PNLite_kDefaultMemorySearchDepth;
    pnlite_kscrw_i_writeMemoryContents(writer, key, address, &limit);
}

/** Look for a hex value in a string and try to write whatever it references.
 *
 * @param writer The writer.
 *
 * @param key The object key, if needed.
 *
 * @param string The string to search.
 */
void pnlite_kscrw_i_writeAddressReferencedByString(
    const PNLite_KSCrashReportWriter *const writer, const char *const key,
    const char *string) {
    uint64_t address = 0;
    if (string == NULL ||
        !pnlite_ksstring_extractHexValue(string, strlen(string), &address)) {
        return;
    }

    int limit = PNLite_kDefaultMemorySearchDepth;
    pnlite_kscrw_i_writeMemoryContents(writer, key, (uintptr_t)address, &limit);
}

#pragma mark Backtrace

/** Write a backtrace entry to the report.
 *
 * @param writer The writer.
 *
 * @param key The object key, if needed.
 *
 * @param address The memory address.
 *
 * @param info Information about the nearest symbols to the address.
 */
void pnlite_kscrw_i_writeBacktraceEntry(
    const PNLite_KSCrashReportWriter *const writer, const char *const key,
    const uintptr_t address, const Dl_info *const info) {
    writer->beginObject(writer, key);
    {
        if (info->dli_fname != NULL) {
            writer->addStringElement(writer, PNLite_KSCrashField_ObjectName,
                                     pnlite_ksfulastPathEntry(info->dli_fname));
        }
        writer->addUIntegerElement(writer, PNLite_KSCrashField_ObjectAddr,
                                   (uintptr_t)info->dli_fbase);
        if (info->dli_sname != NULL) {
            const char *sname = info->dli_sname;
            writer->addStringElement(writer, PNLite_KSCrashField_SymbolName,
                                     sname);
        }
        writer->addUIntegerElement(writer, PNLite_KSCrashField_SymbolAddr,
                                   (uintptr_t)info->dli_saddr);
        writer->addUIntegerElement(writer, PNLite_KSCrashField_InstructionAddr,
                                   address);
    }
    writer->endContainer(writer);
}

/** Write a backtrace to the report.
 *
 * @param writer The writer to write the backtrace to.
 *
 * @param key The object key, if needed.
 *
 * @param backtrace The backtrace to write.
 *
 * @param backtraceLength Length of the backtrace.
 *
 * @param skippedEntries The number of entries that were skipped before the
 *                       beginning of backtrace.
 */
void pnlite_kscrw_i_writeBacktrace(const PNLite_KSCrashReportWriter *const writer,
                                const char *const key,
                                const uintptr_t *const backtrace,
                                const int backtraceLength,
                                const int skippedEntries) {
    writer->beginObject(writer, key);
    {
        writer->beginArray(writer, PNLite_KSCrashField_Contents);
        {
            if (backtraceLength > 0) {
                Dl_info symbolicated[backtraceLength];
                pnlite_ksbt_symbolicate(backtrace, symbolicated, backtraceLength,
                                     skippedEntries);

                for (int i = 0; i < backtraceLength; i++) {
                    pnlite_kscrw_i_writeBacktraceEntry(writer, NULL, backtrace[i],
                                                    &symbolicated[i]);
                }
            }
        }
        writer->endContainer(writer);
        writer->addIntegerElement(writer, PNLite_KSCrashField_Skipped,
                                  skippedEntries);
    }
    writer->endContainer(writer);
}

#pragma mark Stack

/** Write a dump of the stack contents to the report.
 *
 * @param writer The writer.
 *
 * @param key The object key, if needed.
 *
 * @param machineContext The context to retrieve the stack from.
 *
 * @param isStackOverflow If true, the stack has overflowed.
 */
void pnlite_kscrw_i_writeStackContents(
    const PNLite_KSCrashReportWriter *const writer, const char *const key,
    const PNLite_STRUCT_MCONTEXT_L *const machineContext,
    const bool isStackOverflow) {
    uintptr_t sp = pnlite_ksmachstackPointer(machineContext);
    if ((void *)sp == NULL) {
        return;
    }

    uintptr_t lowAddress =
        sp + (uintptr_t)(PNLite_kStackContentsPushedDistance * (int)sizeof(sp) *
                         pnlite_ksmachstackGrowDirection() * -1);
    uintptr_t highAddress =
        sp + (uintptr_t)(PNLite_kStackContentsPoppedDistance * (int)sizeof(sp) *
                         pnlite_ksmachstackGrowDirection());
    if (highAddress < lowAddress) {
        uintptr_t tmp = lowAddress;
        lowAddress = highAddress;
        highAddress = tmp;
    }
    writer->beginObject(writer, key);
    {
        writer->addStringElement(writer, PNLite_KSCrashField_GrowDirection,
                                 pnlite_ksmachstackGrowDirection() > 0 ? "+"
                                                                    : "-");
        writer->addUIntegerElement(writer, PNLite_KSCrashField_DumpStart,
                                   lowAddress);
        writer->addUIntegerElement(writer, PNLite_KSCrashField_DumpEnd,
                                   highAddress);
        writer->addUIntegerElement(writer, PNLite_KSCrashField_StackPtr, sp);
        writer->addBooleanElement(writer, PNLite_KSCrashField_Overflow,
                                  isStackOverflow);
        uint8_t stackBuffer[PNLite_kStackContentsTotalDistance * sizeof(sp)];
        size_t copyLength = highAddress - lowAddress;
        if (pnlite_ksmachcopyMem((void *)lowAddress, stackBuffer, copyLength) ==
            KERN_SUCCESS) {
            writer->addDataElement(writer, PNLite_KSCrashField_Contents,
                                   (void *)stackBuffer, copyLength);
        } else {
            writer->addStringElement(writer, PNLite_KSCrashField_Error,
                                     "Stack contents not accessible");
        }
    }
    writer->endContainer(writer);
}

/** Write any notable addresses near the stack pointer (above and below).
 *
 * @param writer The writer.
 *
 * @param machineContext The context to retrieve the stack from.
 *
 * @param backDistance The distance towards the beginning of the stack to check.
 *
 * @param forwardDistance The distance past the end of the stack to check.
 */
void pnlite_kscrw_i_writeNotableStackContents(
    const PNLite_KSCrashReportWriter *const writer,
    const PNLite_STRUCT_MCONTEXT_L *const machineContext, const int backDistance,
    const int forwardDistance) {
    uintptr_t sp = pnlite_ksmachstackPointer(machineContext);
    if ((void *)sp == NULL) {
        return;
    }

    uintptr_t lowAddress =
        sp + (uintptr_t)(backDistance * (int)sizeof(sp) *
                         pnlite_ksmachstackGrowDirection() * -1);
    uintptr_t highAddress = sp + (uintptr_t)(forwardDistance * (int)sizeof(sp) *
                                             pnlite_ksmachstackGrowDirection());
    if (highAddress < lowAddress) {
        uintptr_t tmp = lowAddress;
        lowAddress = highAddress;
        highAddress = tmp;
    }
    uintptr_t contentsAsPointer;
    char nameBuffer[40];
    for (uintptr_t address = lowAddress; address < highAddress;
         address += sizeof(address)) {
        if (pnlite_ksmachcopyMem((void *)address, &contentsAsPointer,
                              sizeof(contentsAsPointer)) == KERN_SUCCESS) {
            sprintf(nameBuffer, "stack@%p", (void *)address);
            pnlite_kscrw_i_writeMemoryContentsIfNotable(writer, nameBuffer,
                                                     contentsAsPointer);
        }
    }
}

#pragma mark Registers

/** Write the contents of all regular registers to the report.
 *
 * @param writer The writer.
 *
 * @param key The object key, if needed.
 *
 * @param machineContext The context to retrieve the registers from.
 */
void pnlite_kscrw_i_writeBasicRegisters(
    const PNLite_KSCrashReportWriter *const writer, const char *const key,
    const PNLite_STRUCT_MCONTEXT_L *const machineContext) {
    char registerNameBuff[30];
    const char *registerName;
    writer->beginObject(writer, key);
    {
        const int numRegisters = pnlite_ksmachnumRegisters();
        for (int reg = 0; reg < numRegisters; reg++) {
            registerName = pnlite_ksmachregisterName(reg);
            if (registerName == NULL) {
                snprintf(registerNameBuff, sizeof(registerNameBuff), "r%d",
                         reg);
                registerName = registerNameBuff;
            }
            writer->addUIntegerElement(
                writer, registerName,
                pnlite_ksmachregisterValue(machineContext, reg));
        }
    }
    writer->endContainer(writer);
}

/** Write the contents of all exception registers to the report.
 *
 * @param writer The writer.
 *
 * @param key The object key, if needed.
 *
 * @param machineContext The context to retrieve the registers from.
 */
void pnlite_kscrw_i_writeExceptionRegisters(
    const PNLite_KSCrashReportWriter *const writer, const char *const key,
    const PNLite_STRUCT_MCONTEXT_L *const machineContext) {
    char registerNameBuff[30];
    const char *registerName;
    writer->beginObject(writer, key);
    {
        const int numRegisters = pnlite_ksmachnumExceptionRegisters();
        for (int reg = 0; reg < numRegisters; reg++) {
            registerName = pnlite_ksmachexceptionRegisterName(reg);
            if (registerName == NULL) {
                snprintf(registerNameBuff, sizeof(registerNameBuff), "r%d",
                         reg);
                registerName = registerNameBuff;
            }
            writer->addUIntegerElement(
                writer, registerName,
                pnlite_ksmachexceptionRegisterValue(machineContext, reg));
        }
    }
    writer->endContainer(writer);
}

/** Write all applicable registers.
 *
 * @param writer The writer.
 *
 * @param key The object key, if needed.
 *
 * @param machineContext The context to retrieve the registers from.
 *
 * @param isCrashedContext If true, this context represents the crashing thread.
 */
void pnlite_kscrw_i_writeRegisters(
    const PNLite_KSCrashReportWriter *const writer, const char *const key,
    const PNLite_STRUCT_MCONTEXT_L *const machineContext,
    const bool isCrashedContext) {
    writer->beginObject(writer, key);
    {
        pnlite_kscrw_i_writeBasicRegisters(writer, PNLite_KSCrashField_Basic,
                                        machineContext);
        if (isCrashedContext) {
            pnlite_kscrw_i_writeExceptionRegisters(
                writer, PNLite_KSCrashField_Exception, machineContext);
        }
    }
    writer->endContainer(writer);
}

/** Write any notable addresses contained in the CPU registers.
 *
 * @param writer The writer.
 *
 * @param machineContext The context to retrieve the registers from.
 */
void pnlite_kscrw_i_writeNotableRegisters(
    const PNLite_KSCrashReportWriter *const writer,
    const PNLite_STRUCT_MCONTEXT_L *const machineContext) {
    char registerNameBuff[30];
    const char *registerName;
    const int numRegisters = pnlite_ksmachnumRegisters();
    for (int reg = 0; reg < numRegisters; reg++) {
        registerName = pnlite_ksmachregisterName(reg);
        if (registerName == NULL) {
            snprintf(registerNameBuff, sizeof(registerNameBuff), "r%d", reg);
            registerName = registerNameBuff;
        }
        pnlite_kscrw_i_writeMemoryContentsIfNotable(
            writer, registerName,
            (uintptr_t)pnlite_ksmachregisterValue(machineContext, reg));
    }
}

#pragma mark Thread-specific

/** Write any notable addresses in the stack or registers to the report.
 *
 * @param writer The writer.
 *
 * @param key The object key, if needed.
 *
 * @param machineContext The context to retrieve the registers from.
 */
void pnlite_kscrw_i_writeNotableAddresses(
    const PNLite_KSCrashReportWriter *const writer, const char *const key,
    const PNLite_STRUCT_MCONTEXT_L *const machineContext) {
    writer->beginObject(writer, key);
    {
        pnlite_kscrw_i_writeNotableRegisters(writer, machineContext);
        pnlite_kscrw_i_writeNotableStackContents(
            writer, machineContext, PNLite_kStackNotableSearchBackDistance,
            PNLite_kStackNotableSearchForwardDistance);
    }
    writer->endContainer(writer);
}

/** Write information about a thread to the report.
 *
 * @param writer The writer.
 *
 * @param key The object key, if needed.
 *
 * @param crash The crash handler context.
 *
 * @param thread The thread to write about.
 *
 * @param index The thread's index relative to all threads.
 *
 * @param writeNotableAddresses If true, write any notable addresses found.
 */
void pnlite_kscrw_i_writeThread(const PNLite_KSCrashReportWriter *const writer,
                             const char *const key,
                             const PNLite_KSCrash_SentryContext *const crash,
                             const thread_t thread, const int index,
                             const bool writeNotableAddresses,
                             const bool searchThreadNames,
                             const bool searchQueueNames) {
    bool isCrashedThread = thread == crash->offendingThread;
    char nameBuffer[128];
    PNLite_STRUCT_MCONTEXT_L machineContextBuffer;
    uintptr_t backtraceBuffer[PNLite_kMaxBacktraceDepth];
    int backtraceLength = sizeof(backtraceBuffer) / sizeof(*backtraceBuffer);
    int skippedEntries = 0;

    PNLite_STRUCT_MCONTEXT_L *machineContext =
        pnlite_kscrw_i_getMachineContext(crash, thread, &machineContextBuffer);

    uintptr_t *backtrace =
        pnlite_kscrw_i_getBacktrace(crash, thread, machineContext, backtraceBuffer,
                                 &backtraceLength, &skippedEntries);

    writer->beginObject(writer, key);
    {
        if (backtrace != NULL) {
            pnlite_kscrw_i_writeBacktrace(writer, PNLite_KSCrashField_Backtrace,
                                       backtrace, backtraceLength,
                                       skippedEntries);
        }
        if (machineContext != NULL) {
            pnlite_kscrw_i_writeRegisters(writer, PNLite_KSCrashField_Registers,
                                       machineContext, isCrashedThread);
        }
        writer->addIntegerElement(writer, PNLite_KSCrashField_Index, index);
        if (searchThreadNames) {
            if (pnlite_ksmachgetThreadName(thread, nameBuffer,
                                        sizeof(nameBuffer)) &&
                nameBuffer[0] != 0) {
                writer->addStringElement(writer, PNLite_KSCrashField_Name,
                                         nameBuffer);
            }
        }
        if (searchQueueNames) {
            if (pnlite_ksmachgetThreadQueueName(thread, nameBuffer,
                                             sizeof(nameBuffer)) &&
                nameBuffer[0] != 0) {
                writer->addStringElement(writer, PNLite_KSCrashField_DispatchQueue,
                                         nameBuffer);
            }
        }
        writer->addBooleanElement(writer, PNLite_KSCrashField_Crashed,
                                  isCrashedThread);
        writer->addBooleanElement(writer, PNLite_KSCrashField_CurrentThread,
                                  thread == pnlite_ksmachthread_self());
        if (isCrashedThread && machineContext != NULL) {
            pnlite_kscrw_i_writeStackContents(writer, PNLite_KSCrashField_Stack,
                                           machineContext, skippedEntries > 0);
            if (writeNotableAddresses) {
                pnlite_kscrw_i_writeNotableAddresses(
                    writer, PNLite_KSCrashField_NotableAddresses, machineContext);
            }
        }
    }
    writer->endContainer(writer);
}

/** Write information about all threads to the report.
 *
 * @param writer The writer.
 *
 * @param key The object key, if needed.
 *
 * @param crash The crash handler context.
 */
void pnlite_kscrw_i_writeAllThreads(const PNLite_KSCrashReportWriter *const writer,
                                 const char *const key,
                                 const PNLite_KSCrash_SentryContext *const crash,
                                 bool writeNotableAddresses,
                                 bool searchThreadNames,
                                 bool searchQueueNames) {
    const task_t thisTask = mach_task_self();
    thread_act_array_t threads;
    mach_msg_type_number_t numThreads;
    kern_return_t kr;

    if ((kr = task_threads(thisTask, &threads, &numThreads)) != KERN_SUCCESS) {
        PNLite_KSLOG_ERROR("task_threads: %s", mach_error_string(kr));
        return;
    }

    // Fetch info for all threads.
    writer->beginArray(writer, key);
    {
        for (mach_msg_type_number_t i = 0; i < numThreads; i++) {
            pnlite_kscrw_i_writeThread(writer, NULL, crash, threads[i], (int)i,
                                    writeNotableAddresses, searchThreadNames,
                                    searchQueueNames);
        }
    }
    writer->endContainer(writer);

    // Clean up.
    for (mach_msg_type_number_t i = 0; i < numThreads; i++) {
        mach_port_deallocate(thisTask, threads[i]);
    }
    vm_deallocate(thisTask, (vm_address_t)threads,
                  sizeof(thread_t) * numThreads);
}

/** Get the index of a thread.
 *
 * @param thread The thread.
 *
 * @return The thread's index, or -1 if it couldn't be determined.
 */
int pnlite_kscrw_i_threadIndex(const thread_t thread) {
    int index = -1;
    const task_t thisTask = mach_task_self();
    thread_act_array_t threads;
    mach_msg_type_number_t numThreads;
    kern_return_t kr;

    if ((kr = task_threads(thisTask, &threads, &numThreads)) != KERN_SUCCESS) {
        PNLite_KSLOG_ERROR("task_threads: %s", mach_error_string(kr));
        return -1;
    }

    for (mach_msg_type_number_t i = 0; i < numThreads; i++) {
        if (threads[i] == thread) {
            index = (int)i;
            break;
        }
    }

    // Clean up.
    for (mach_msg_type_number_t i = 0; i < numThreads; i++) {
        mach_port_deallocate(thisTask, threads[i]);
    }
    vm_deallocate(thisTask, (vm_address_t)threads,
                  sizeof(thread_t) * numThreads);

    return index;
}

#pragma mark Global Report Data

/** Write information about a binary image to the report.
 *
 * @param writer The writer.
 *
 * @param key The object key, if needed.
 *
 * @param index Which image to write about.
 */
void pnlite_kscrw_i_writeBinaryImage(const PNLite_KSCrashReportWriter *const writer,
                                  const char *const key, const uint32_t index) {
    const struct mach_header *header = _dyld_get_image_header(index);
    if (header == NULL) {
        return;
    }

    uintptr_t cmdPtr = pnlite_ksdlfirstCmdAfterHeader(header);
    if (cmdPtr == 0) {
        return;
    }

    // Look for the TEXT segment to get the image size.
    // Also look for a UUID command.
    uint64_t imageSize = 0;
    uint64_t imageVmAddr = 0;
    uint8_t *uuid = NULL;

    for (uint32_t iCmd = 0; iCmd < header->ncmds; iCmd++) {
        struct load_command *loadCmd = (struct load_command *)cmdPtr;
        switch (loadCmd->cmd) {
        case LC_SEGMENT: {
            struct segment_command *segCmd = (struct segment_command *)cmdPtr;
            if (strcmp(segCmd->segname, SEG_TEXT) == 0) {
                imageSize = segCmd->vmsize;
                imageVmAddr = segCmd->vmaddr;
            }
            break;
        }
        case LC_SEGMENT_64: {
            struct segment_command_64 *segCmd =
                (struct segment_command_64 *)cmdPtr;
            if (strcmp(segCmd->segname, SEG_TEXT) == 0) {
                imageSize = segCmd->vmsize;
                imageVmAddr = segCmd->vmaddr;
            }
            break;
        }
        case LC_UUID: {
            struct uuid_command *uuidCmd = (struct uuid_command *)cmdPtr;
            uuid = uuidCmd->uuid;
            break;
        }
        }
        cmdPtr += loadCmd->cmdsize;
    }

    writer->beginObject(writer, key);
    {
        writer->addUIntegerElement(writer, PNLite_KSCrashField_ImageAddress,
                                   (uintptr_t)header);
        writer->addUIntegerElement(writer, PNLite_KSCrashField_ImageVmAddress,
                                   imageVmAddr);
        writer->addUIntegerElement(writer, PNLite_KSCrashField_ImageSize,
                                   imageSize);
        writer->addStringElement(writer, PNLite_KSCrashField_Name,
                                 _dyld_get_image_name(index));
        writer->addUUIDElement(writer, PNLite_KSCrashField_UUID, uuid);
        writer->addIntegerElement(writer, PNLite_KSCrashField_CPUType,
                                  header->cputype);
        writer->addIntegerElement(writer, PNLite_KSCrashField_CPUSubType,
                                  header->cpusubtype);
    }
    writer->endContainer(writer);
}

/** Write information about all images to the report.
 *
 * @param writer The writer.
 *
 * @param key The object key, if needed.
 */
void pnlite_kscrw_i_writeBinaryImages(const PNLite_KSCrashReportWriter *const writer,
                                   const char *const key) {
    const uint32_t imageCount = _dyld_image_count();

    writer->beginArray(writer, key);
    {
        for (uint32_t iImg = 0; iImg < imageCount; iImg++) {
            pnlite_kscrw_i_writeBinaryImage(writer, NULL, iImg);
        }
    }
    writer->endContainer(writer);
}

/** Write information about system memory to the report.
 *
 * @param writer The writer.
 *
 * @param key The object key, if needed.
 */
void pnlite_kscrw_i_writeMemoryInfo(const PNLite_KSCrashReportWriter *const writer,
                                 const char *const key) {
    writer->beginObject(writer, key);
    {
        writer->addUIntegerElement(writer, PNLite_KSCrashField_Usable,
                                   pnlite_ksmachusableMemory());
        writer->addUIntegerElement(writer, PNLite_KSCrashField_Free,
                                   pnlite_ksmachfreeMemory());
    }
    writer->endContainer(writer);
}

/** Write information about the error leading to the crash to the report.
 *
 * @param writer The writer.
 *
 * @param key The object key, if needed.
 *
 * @param crash The crash handler context.
 */
void pnlite_kscrw_i_writeError(const PNLite_KSCrashReportWriter *const writer,
                            const char *const key,
                            const PNLite_KSCrash_SentryContext *const crash) {
    int machExceptionType = 0;
    kern_return_t machCode = 0;
    kern_return_t machSubCode = 0;
    int sigNum = 0;
    int sigCode = 0;
    const char *exceptionName = NULL;
    const char *crashReason = NULL;

    // Gather common info.
    switch (crash->crashType) {
    case PNLite_KSCrashTypeMainThreadDeadlock:
        break;
    case PNLite_KSCrashTypeMachException:
        machExceptionType = crash->mach.type;
        machCode = (kern_return_t)crash->mach.code;
        if (machCode == KERN_PROTECTION_FAILURE && crash->isStackOverflow) {
            // A stack overflow should return KERN_INVALID_ADDRESS, but
            // when a stack blasts through the guard pages at the top of the
            // stack, it generates KERN_PROTECTION_FAILURE. Correct for this.
            machCode = KERN_INVALID_ADDRESS;
        }
        machSubCode = (kern_return_t)crash->mach.subcode;

        sigNum =
            pnlite_kssignal_signalForMachException(machExceptionType, machCode);
        break;
    case PNLite_KSCrashTypeCPPException:
        machExceptionType = EXC_CRASH;
        sigNum = SIGABRT;
        crashReason = crash->crashReason;
        exceptionName = crash->CPPException.name;
        break;
    case PNLite_KSCrashTypeNSException:
        machExceptionType = EXC_CRASH;
        sigNum = SIGABRT;
        exceptionName = crash->NSException.name;
        crashReason = crash->crashReason;
        break;
    case PNLite_KSCrashTypeSignal:
        sigNum = crash->signal.signalInfo->si_signo;
        sigCode = crash->signal.signalInfo->si_code;
        machExceptionType = pnlite_kssignal_machExceptionForSignal(sigNum);
        break;
    case PNLite_KSCrashTypeUserReported:
        machExceptionType = EXC_CRASH;
        sigNum = SIGABRT;
        crashReason = crash->crashReason;
        break;
    }

    const char *machExceptionName = pnlite_ksmachexceptionName(machExceptionType);
    const char *machCodeName =
        machCode == 0 ? NULL : pnlite_ksmachkernelReturnCodeName(machCode);
    const char *sigName = pnlite_kssignal_signalName(sigNum);
    const char *sigCodeName = pnlite_kssignal_signalCodeName(sigNum, sigCode);

    writer->beginObject(writer, key);
    {

        if (PNLite_KSCrashTypeUserReported != crash->crashType) {
            writer->addUIntegerElement(writer, PNLite_KSCrashField_Address,
                                       crash->faultAddress);
        }

        if (crashReason != NULL) {
            writer->addStringElement(writer, PNLite_KSCrashField_Reason,
                                     crashReason);
        }


        // Gather specific info.
        switch (crash->crashType) {
        case PNLite_KSCrashTypeMainThreadDeadlock:
            writer->addStringElement(writer, PNLite_KSCrashField_Type,
                                     PNLite_KSCrashExcType_Deadlock);
            break;

        case PNLite_KSCrashTypeMachException:
            writer->beginObject(writer, PNLite_KSCrashField_Mach);
            {
                writer->addUIntegerElement(writer, PNLite_KSCrashField_Exception,
                                           (unsigned)machExceptionType);
                if (machExceptionName != NULL) {
                    writer->addStringElement(writer, PNLite_KSCrashField_ExceptionName,
                                             machExceptionName);
                }
                writer->addUIntegerElement(writer, PNLite_KSCrashField_Code,
                                           (unsigned)machCode);
                if (machCodeName != NULL) {
                    writer->addStringElement(writer, PNLite_KSCrashField_CodeName,
                                             machCodeName);
                }
                writer->addUIntegerElement(writer, PNLite_KSCrashField_Subcode,
                                           (unsigned)machSubCode);
            }
            writer->endContainer(writer);
            writer->addStringElement(writer, PNLite_KSCrashField_Type,
                                     PNLite_KSCrashExcType_Mach);
            break;

        case PNLite_KSCrashTypeCPPException: {
            writer->addStringElement(writer, PNLite_KSCrashField_Type,
                                     PNLite_KSCrashExcType_CPPException);
            writer->beginObject(writer, PNLite_KSCrashField_CPPException);
            {
                writer->addStringElement(writer, PNLite_KSCrashField_Name,
                                         exceptionName);
            }
            writer->endContainer(writer);
            break;
        }
        case PNLite_KSCrashTypeNSException: {
            writer->addStringElement(writer, PNLite_KSCrashField_Type,
                                     PNLite_KSCrashExcType_NSException);
            writer->beginObject(writer, PNLite_KSCrashField_NSException);
            {
                writer->addStringElement(writer, PNLite_KSCrashField_Name,
                                         exceptionName);
                pnlite_kscrw_i_writeAddressReferencedByString(
                    writer, PNLite_KSCrashField_ReferencedObject, crashReason);
            }
            writer->endContainer(writer);
            break;
        }
        case PNLite_KSCrashTypeSignal:
            writer->beginObject(writer, PNLite_KSCrashField_Signal);
            {
                writer->addUIntegerElement(writer, PNLite_KSCrashField_Signal,
                                           (unsigned)sigNum);
                if (sigName != NULL) {
                    writer->addStringElement(writer, PNLite_KSCrashField_Name,
                                             sigName);
                }
                writer->addUIntegerElement(writer, PNLite_KSCrashField_Code,
                                           (unsigned)sigCode);
                if (sigCodeName != NULL) {
                    writer->addStringElement(writer, PNLite_KSCrashField_CodeName,
                                             sigCodeName);
                }
            }
            writer->endContainer(writer);
            writer->addStringElement(writer, PNLite_KSCrashField_Type,
                                     PNLite_KSCrashExcType_Signal);
            break;

        case PNLite_KSCrashTypeUserReported: {
            writer->addStringElement(writer, PNLite_KSCrashField_Type,
                                     PNLite_KSCrashExcType_User);
            writer->beginObject(writer, PNLite_KSCrashField_UserReported);
            {
                writer->addStringElement(writer, PNLite_KSCrashField_Name,
                                         crash->userException.name);
                if (crash->userException.language != NULL) {
                    writer->addStringElement(writer, PNLite_KSCrashField_Language,
                                             crash->userException.language);
                }
                if (crash->userException.lineOfCode != NULL) {
                    writer->addStringElement(writer,
                                             PNLite_KSCrashField_LineOfCode,
                                             crash->userException.lineOfCode);
                }
                if (crash->userException.customStackTrace != NULL) {
                    writer->addJSONElement(
                        writer, PNLite_KSCrashField_Backtrace,
                        crash->userException.customStackTrace);
                }
            }
            writer->endContainer(writer);
            break;
        }
        }
    }
    writer->endContainer(writer);
}

/** Write information about app runtime, etc to the report.
 *
 * @param writer The writer.
 *
 * @param key The object key, if needed.
 *
 * @param state The persistent crash handler state.
 */
void pnlite_kscrw_i_writeAppStats(const PNLite_KSCrashReportWriter *const writer,
                               const char *const key,
                               PNLite_KSCrash_State *state) {
    writer->beginObject(writer, key);
    {
        writer->addBooleanElement(writer, PNLite_KSCrashField_AppActive,
                                  state->applicationIsActive);
        writer->addBooleanElement(writer, PNLite_KSCrashField_AppInFG,
                                  state->applicationIsInForeground);

        writer->addIntegerElement(writer, PNLite_KSCrashField_LaunchesSinceCrash,
                                  state->launchesSinceLastCrash);
        writer->addIntegerElement(writer, PNLite_KSCrashField_SessionsSinceCrash,
                                  state->sessionsSinceLastCrash);
        writer->addFloatingPointElement(writer,
                                        PNLite_KSCrashField_ActiveTimeSinceCrash,
                                        state->activeDurationSinceLastCrash);
        writer->addFloatingPointElement(
            writer, PNLite_KSCrashField_BGTimeSinceCrash,
            state->backgroundDurationSinceLastCrash);

        writer->addIntegerElement(writer, PNLite_KSCrashField_SessionsSinceLaunch,
                                  state->sessionsSinceLaunch);
        writer->addFloatingPointElement(writer,
                                        PNLite_KSCrashField_ActiveTimeSinceLaunch,
                                        state->activeDurationSinceLaunch);
        writer->addFloatingPointElement(writer,
                                        PNLite_KSCrashField_BGTimeSinceLaunch,
                                        state->backgroundDurationSinceLaunch);
    }
    writer->endContainer(writer);
}

/** Write information about this process.
 *
 * @param writer The writer.
 *
 * @param key The object key, if needed.
 */
void pnlite_kscrw_i_writeProcessState(const PNLite_KSCrashReportWriter *const writer,
                                   const char *const key) {
    writer->beginObject(writer, key);
    {
        const void *excAddress = pnlite_kszombie_lastDeallocedNSExceptionAddress();
        if (excAddress != NULL) {
            writer->beginObject(writer,
                                PNLite_KSCrashField_LastDeallocedNSException);
            {
                writer->addUIntegerElement(writer, PNLite_KSCrashField_Address,
                                           (uintptr_t)excAddress);
                writer->addStringElement(
                    writer, PNLite_KSCrashField_Name,
                    pnlite_kszombie_lastDeallocedNSExceptionName());
                writer->addStringElement(
                    writer, PNLite_KSCrashField_Reason,
                    pnlite_kszombie_lastDeallocedNSExceptionReason());
                pnlite_kscrw_i_writeAddressReferencedByString(
                    writer, PNLite_KSCrashField_ReferencedObject,
                    pnlite_kszombie_lastDeallocedNSExceptionReason());
            }
            writer->endContainer(writer);
        }
    }
    writer->endContainer(writer);
}

/** Write basic report information.
 *
 * @param writer The writer.
 *
 * @param key The object key, if needed.
 *
 * @param type The report type.
 *
 * @param reportID The report ID.
 */
void pnlite_kscrw_i_writeReportInfo(const PNLite_KSCrashReportWriter *const writer,
                                 const char *const key, const char *const type,
                                 const char *const reportID,
                                 const char *const processName) {
    writer->beginObject(writer, key);
    {
        writer->addStringElement(writer, PNLite_KSCrashField_Version,
                                 PNLite_KSCRASH_REPORT_VERSION);
        writer->addStringElement(writer, PNLite_KSCrashField_ID, reportID);
        writer->addStringElement(writer, PNLite_KSCrashField_ProcessName,
                                 processName);
        writer->addIntegerElement(writer, PNLite_KSCrashField_Timestamp,
                                  time(NULL));
        writer->addStringElement(writer, PNLite_KSCrashField_Type, type);
    }
    writer->endContainer(writer);
}

#pragma mark Setup

/** Prepare a report writer for use.
 *
 * @oaram writer The writer to prepare.
 *
 * @param context JSON writer contextual information.
 */
void pnlite_kscrw_i_prepareReportWriter(PNLite_KSCrashReportWriter *const writer,
                                     PNLite_KSJSONEncodeContext *const context) {
    writer->addBooleanElement = pnlite_kscrw_i_addBooleanElement;
    writer->addFloatingPointElement = pnlite_kscrw_i_addFloatingPointElement;
    writer->addIntegerElement = pnlite_kscrw_i_addIntegerElement;
    writer->addUIntegerElement = pnlite_kscrw_i_addUIntegerElement;
    writer->addStringElement = pnlite_kscrw_i_addStringElement;
    writer->addTextFileElement = pnlite_kscrw_i_addTextFileElement;
    writer->addJSONFileElement = pnlite_kscrw_i_addJSONElementFromFile;
    writer->addDataElement = pnlite_kscrw_i_addDataElement;
    writer->beginDataElement = pnlite_kscrw_i_beginDataElement;
    writer->appendDataElement = pnlite_kscrw_i_appendDataElement;
    writer->endDataElement = pnlite_kscrw_i_endDataElement;
    writer->addUUIDElement = pnlite_kscrw_i_addUUIDElement;
    writer->addJSONElement = pnlite_kscrw_i_addJSONElement;
    writer->beginObject = pnlite_kscrw_i_beginObject;
    writer->beginArray = pnlite_kscrw_i_beginArray;
    writer->endContainer = pnlite_kscrw_i_endContainer;
    writer->context = context;
}

/** Open the crash report file.
 *
 * @param path The path to the file.
 *
 * @return The file descriptor, or -1 if an error occurred.
 */
int pnlite_kscrw_i_openCrashReportFile(const char *const path) {
    int fd = open(path, O_RDWR | O_CREAT | O_EXCL, 0644);
    if (fd < 0) {
        PNLite_KSLOG_ERROR("Could not open crash report file %s: %s", path,
                        strerror(errno));
    }
    return fd;
}

/** Record whether the crashed thread had a stack overflow or not.
 *
 * @param crashContext the context.
 */
void pnlite_kscrw_i_updateStackOverflowStatus(
    PNLite_KSCrash_Context *const crashContext) {
    // TODO: This feels weird. Shouldn't be mutating the context.
    if (pnlite_kscrw_i_isStackOverflow(&crashContext->crash,
                                    crashContext->crash.offendingThread)) {
        PNLite_KSLOG_TRACE("Stack overflow detected.");
        crashContext->crash.isStackOverflow = true;
    }
}

void pnlite_kscrw_i_callUserCrashHandler(PNLite_KSCrash_Context *const crashContext,
                                      PNLite_KSCrashReportWriter *writer) {
    crashContext->config.onCrashNotify(writer);
}

// ============================================================================
#pragma mark - Main API -
// ============================================================================

void pnlite_kscrashreport_writeMinimalReport(
    PNLite_KSCrash_Context *const crashContext, const char *const path) {
    PNLite_KSLOG_INFO("Writing minimal crash report to %s", path);

    int fd = pnlite_kscrw_i_openCrashReportFile(path);
    if (fd < 0) {
        return;
    }

    pnlite_g_introspectionRules = &crashContext->config.introspectionRules;

    pnlite_kscrw_i_updateStackOverflowStatus(crashContext);

    PNLite_KSJSONEncodeContext jsonContext;
    jsonContext.userData = &fd;
    PNLite_KSCrashReportWriter concreteWriter;
    PNLite_KSCrashReportWriter *writer = &concreteWriter;
    pnlite_kscrw_i_prepareReportWriter(writer, &jsonContext);

    pnlite_ksjsonbeginEncode(pnlite_getJsonContext(writer), true,
                          pnlite_kscrw_i_addJSONData, &fd);

    writer->beginObject(writer, PNLite_KSCrashField_Report);
    {
        pnlite_kscrw_i_writeReportInfo(
            writer, PNLite_KSCrashField_Report, PNLite_KSCrashReportType_Minimal,
            crashContext->config.crashID, crashContext->config.processName);

        writer->beginObject(writer, PNLite_KSCrashField_Crash);
        {
            pnlite_kscrw_i_writeThread(
                writer, PNLite_KSCrashField_CrashedThread, &crashContext->crash,
                crashContext->crash.offendingThread,
                pnlite_kscrw_i_threadIndex(crashContext->crash.offendingThread),
                false, false, false);
            pnlite_kscrw_i_writeError(writer, PNLite_KSCrashField_Error,
                                   &crashContext->crash);
        }
        writer->endContainer(writer);
    }
    writer->endContainer(writer);

    pnlite_ksjsonendEncode(pnlite_getJsonContext(writer));

    close(fd);
}

void pnlite_kscrashreport_writeStandardReport(
    PNLite_KSCrash_Context *const crashContext, const char *const path) {
    PNLite_KSLOG_INFO("Writing crash report to %s", path);

    int fd = pnlite_kscrw_i_openCrashReportFile(path);
    if (fd < 0) {
        return;
    }

    pnlite_g_introspectionRules = &crashContext->config.introspectionRules;

    pnlite_kscrw_i_updateStackOverflowStatus(crashContext);

    PNLite_KSJSONEncodeContext jsonContext;
    jsonContext.userData = &fd;
    PNLite_KSCrashReportWriter concreteWriter;
    PNLite_KSCrashReportWriter *writer = &concreteWriter;
    pnlite_kscrw_i_prepareReportWriter(writer, &jsonContext);

    pnlite_ksjsonbeginEncode(pnlite_getJsonContext(writer), true,
                          pnlite_kscrw_i_addJSONData, &fd);

    writer->beginObject(writer, PNLite_KSCrashField_Report);
    {
        pnlite_kscrw_i_writeReportInfo(
            writer, PNLite_KSCrashField_Report, PNLite_KSCrashReportType_Standard,
            crashContext->config.crashID, crashContext->config.processName);

        // Don't write the binary images for user reported crashes to improve
        // performance
        if (crashContext->crash.writeBinaryImagesForUserReported == true ||
            crashContext->crash.crashType != PNLite_KSCrashTypeUserReported) {
            pnlite_kscrw_i_writeBinaryImages(writer,
                                          PNLite_KSCrashField_BinaryImages);
        }

        pnlite_kscrw_i_writeProcessState(writer, PNLite_KSCrashField_ProcessState);

        if (crashContext->config.systemInfoJSON != NULL) {
            pnlite_kscrw_i_addJSONElement(writer, PNLite_KSCrashField_System,
                                       crashContext->config.systemInfoJSON);
        }

        writer->beginObject(writer, PNLite_KSCrashField_SystemAtCrash);
        {
            pnlite_kscrw_i_writeMemoryInfo(writer, PNLite_KSCrashField_Memory);
            pnlite_kscrw_i_writeAppStats(writer, PNLite_KSCrashField_AppStats,
                                      &crashContext->state);
        }
        writer->endContainer(writer);

        if (crashContext->config.userInfoJSON != NULL) {
            pnlite_kscrw_i_addJSONElement(writer, PNLite_KSCrashField_User,
                                       crashContext->config.userInfoJSON);
        }

        writer->beginObject(writer, PNLite_KSCrashField_Crash);
        {
            // Don't write the threads for user reported crashes to improve
            // performance
            if (crashContext->crash.threadTracingEnabled == true ||
                crashContext->crash.crashType != PNLite_KSCrashTypeUserReported) {
                pnlite_kscrw_i_writeAllThreads(
                    writer, PNLite_KSCrashField_Threads, &crashContext->crash,
                    crashContext->config.introspectionRules.enabled,
                    crashContext->config.searchThreadNames,
                    crashContext->config.searchQueueNames);
            }
            pnlite_kscrw_i_writeError(writer, PNLite_KSCrashField_Error,
                    &crashContext->crash);
        }
        writer->endContainer(writer);

        if (crashContext->config.onCrashNotify != NULL) {
            writer->beginObject(writer, PNLite_KSCrashField_UserAtCrash);
            { pnlite_kscrw_i_callUserCrashHandler(crashContext, writer); }
            writer->endContainer(writer);
        }
    }
    writer->endContainer(writer);

    pnlite_ksjsonendEncode(pnlite_getJsonContext(writer));

    if (!pnlite_ksfuflushWriteBuffer(fd)) {
        PNLite_KSLOG_ERROR("Failed to flush write buffer");
    }
    close(fd);
}

void pnlite_kscrashreport_logCrash(const PNLite_KSCrash_Context *const crashContext) {
    const PNLite_KSCrash_SentryContext *crash = &crashContext->crash;
    pnlite_kscrw_i_logCrashType(crash);
    pnlite_kscrw_i_logCrashThreadBacktrace(&crashContext->crash);
}
