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

#import "PNLite_KSCrashSentry_NSException.h"
#import "PNLite_KSCrashSentry_Private.h"
#include "PNLite_KSMach.h"

//#define PNLite_KSLogger_LocalLevel TRACE
#import "PNLite_KSLogger.h"

// ============================================================================
#pragma mark - Globals -
// ============================================================================

/** Flag noting if we've installed our custom handlers or not.
 * It's not fully thread safe, but it's safer than locking and slightly better
 * than nothing.
 */
static volatile sig_atomic_t pnlite_g_installed = 0;

/** The exception handler that was in place before we installed ours. */
static NSUncaughtExceptionHandler *pnlite_g_previousUncaughtExceptionHandler;

/** Context to fill with crash information. */
static PNLite_KSCrash_SentryContext *pnlite_g_context;

// ============================================================================
#pragma mark - Callbacks -
// ============================================================================

// Avoiding static methods due to linker issue.

/** Our custom excepetion handler.
 * Fetch the stack trace from the exception and write a report.
 *
 * @param exception The exception that was raised.
 */
void pnlite_ksnsexc_i_handleException(NSException *exception) {
    PNLite_KSLOG_DEBUG(@"Trapped exception %@", exception);
    if (pnlite_g_installed) {
        bool wasHandlingCrash = pnlite_g_context->handlingCrash;
        pnlite_kscrashsentry_beginHandlingCrash(pnlite_g_context);

        PNLite_KSLOG_DEBUG(
            @"Exception handler is installed. Continuing exception handling.");

        if (wasHandlingCrash) {
            PNLite_KSLOG_INFO(@"Detected crash in the crash reporter. Restoring "
                           @"original handlers.");
            pnlite_g_context->crashedDuringCrashHandling = true;
            pnlite_kscrashsentry_uninstall(PNLite_KSCrashTypeAll);
        }

        PNLite_KSLOG_DEBUG(@"Suspending all threads.");
        pnlite_kscrashsentry_suspendThreads();

        PNLite_KSLOG_DEBUG(@"Filling out context.");
        NSArray *addresses = [exception callStackReturnAddresses];
        NSUInteger numFrames = [addresses count];
        uintptr_t *callstack = malloc(numFrames * sizeof(*callstack));
        for (NSUInteger i = 0; i < numFrames; i++) {
            callstack[i] = [addresses[i] unsignedLongValue];
        }

        pnlite_g_context->crashType = PNLite_KSCrashTypeNSException;
        pnlite_g_context->offendingThread = pnlite_ksmachthread_self();
        pnlite_g_context->registersAreValid = false;
        pnlite_g_context->NSException.name = strdup([[exception name] UTF8String]);
        pnlite_g_context->crashReason = strdup([[exception reason] UTF8String]);
        pnlite_g_context->stackTrace = callstack;
        pnlite_g_context->stackTraceLength = (int)numFrames;

        PNLite_KSLOG_DEBUG(@"Calling main crash handler.");
        pnlite_g_context->onCrash();

        PNLite_KSLOG_DEBUG(
            @"Crash handling complete. Restoring original handlers.");
        pnlite_kscrashsentry_uninstall(PNLite_KSCrashTypeAll);

        if (pnlite_g_previousUncaughtExceptionHandler != NULL) {
            PNLite_KSLOG_DEBUG(@"Calling original exception handler.");
            pnlite_g_previousUncaughtExceptionHandler(exception);
        }
    }
}

// ============================================================================
#pragma mark - API -
// ============================================================================

bool pnlite_kscrashsentry_installNSExceptionHandler(
    PNLite_KSCrash_SentryContext *const context) {
    PNLite_KSLOG_DEBUG(@"Installing NSException handler.");
    if (pnlite_g_installed) {
        PNLite_KSLOG_DEBUG(@"NSException handler already installed.");
        return true;
    }
    pnlite_g_installed = 1;

    pnlite_g_context = context;

    PNLite_KSLOG_DEBUG(@"Backing up original handler.");
    pnlite_g_previousUncaughtExceptionHandler = NSGetUncaughtExceptionHandler();

    PNLite_KSLOG_DEBUG(@"Setting new handler.");
    NSSetUncaughtExceptionHandler(&pnlite_ksnsexc_i_handleException);

    return true;
}

void pnlite_kscrashsentry_uninstallNSExceptionHandler(void) {
    PNLite_KSLOG_DEBUG(@"Uninstalling NSException handler.");
    if (!pnlite_g_installed) {
        PNLite_KSLOG_DEBUG(@"NSException handler was already uninstalled.");
        return;
    }

    PNLite_KSLOG_DEBUG(@"Restoring original handler.");
    NSSetUncaughtExceptionHandler(pnlite_g_previousUncaughtExceptionHandler);
    pnlite_g_installed = 0;
}
