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

/** Keeps watch for crashes and informs via callback when on occurs.
 */

#ifndef HDR_PNLite_KSCrashSentry_h
#define HDR_PNLite_KSCrashSentry_h

#ifdef __cplusplus
extern "C" {
#endif

#include "PNLite_KSArchSpecific.h"
#include "PNLite_KSCrashType.h"

#include <mach/mach_types.h>
#include <signal.h>
#include <stdbool.h>

typedef enum {
    PNLite_KSCrashReservedThreadTypeMachPrimary,
    PNLite_KSCrashReservedThreadTypeMachSecondary,
    PNLite_KSCrashReservedThreadTypeCount
} PNLite_KSCrashReservedTheadType;

typedef struct PNLite_KSCrash_SentryContext {
    // Caller defined values. Caller must fill these out prior to installation.

    /** Called by the crash handler when a crash is detected. */
    void (*onCrash)(void);

    /** If true, will suspend threads for user reported exceptions. */
    bool suspendThreadsForUserReported;

    /** If true, will send reports even if debugger is attached. */
    bool reportWhenDebuggerIsAttached;

    /** If true, will trace threads and report binary images. */
    bool threadTracingEnabled;

    /** If true, will record binary images. */
    bool writeBinaryImagesForUserReported;

    // Implementation defined values. Caller does not initialize these.

    /** Threads reserved by the crash handlers, which must not be suspended. */
    thread_t reservedThreads[PNLite_KSCrashReservedThreadTypeCount];

    /** If true, the crash handling system is currently handling a crash.
     * When false, all values below this field are considered invalid.
     */
    bool handlingCrash;

    /** If true, a second crash occurred while handling a crash. */
    bool crashedDuringCrashHandling;

    /** If true, the registers contain valid information about the crash. */
    bool registersAreValid;

    /** True if the crash system has detected a stack overflow. */
    bool isStackOverflow;

    /** The thread that caused the problem. */
    thread_t offendingThread;

    /** Address that caused the fault. */
    uintptr_t faultAddress;

    /** The type of crash that occurred.
     * This determines which other fields are valid. */
    PNLite_KSCrashType crashType;

    /** Short description of why the crash occurred. */
    const char *crashReason;

    /** The stack trace. */
    uintptr_t *stackTrace;

    /** Length of the stack trace. */
    int stackTraceLength;

    struct {
        /** The mach exception type. */
        int type;

        /** The mach exception code. */
        int64_t code;

        /** The mach exception subcode. */
        int64_t subcode;
    } mach;

    struct {
        /** The exception name. */
        const char *name;

    } NSException;

    struct {
        /** The exception name. */
        const char *name;

    } CPPException;

    struct {
        /** User context information. */
        const void *userContext;

        /** Signal information. */
        const siginfo_t *signalInfo;
    } signal;

    struct {
        /** The exception name. */
        const char *name;

        /** The language the exception occured in. */
        const char *language;

        /** The line of code where the exception occurred. Can be NULL. */
        const char *lineOfCode;

        /** The user-supplied JSON encoded stack trace. */
        const char *customStackTrace;
    } userException;

} PNLite_KSCrash_SentryContext;

/** Install crash sentry.
 *
 * @param context Contextual information for the crash handlers.
 *
 * @param crashTypes The crash types to install handlers for.
 *
 * @param onCrash Function to call when a crash occurs.
 *
 * @return which crash handlers were installed successfully.
 */
PNLite_KSCrashType
pnlite_kscrashsentry_installWithContext(PNLite_KSCrash_SentryContext *context,
                                     PNLite_KSCrashType crashTypes,
                                     void (*onCrash)(void));

/** Uninstall crash sentry.
 *
 * @param crashTypes The crash types to install handlers for.
 */
void pnlite_kscrashsentry_uninstall(PNLite_KSCrashType crashTypes);

#ifdef __cplusplus
}
#endif

#endif // HDR_KSCrashSentry_h
