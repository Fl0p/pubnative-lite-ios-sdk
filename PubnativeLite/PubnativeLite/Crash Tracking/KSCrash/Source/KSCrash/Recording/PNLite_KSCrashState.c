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

#include "PNLite_KSCrashState.h"

#include "PNLite_KSFileUtils.h"
#include "PNLite_KSJSONCodec.h"
#include "PNLite_KSMach.h"

//#define PNLite_KSLogger_LocalLevel TRACE
#include "PNLite_KSLogger.h"

#include <errno.h>
#include <fcntl.h>
#include <mach/mach_time.h>
#include <stdlib.h>
#include <unistd.h>

// ============================================================================
#pragma mark - Constants -
// ============================================================================

#define PNLite_kFormatVersion 1

#define PNLite_kKeyFormatVersion "version"
#define PNLite_kKeyCrashedLastLaunch "crashedLastLaunch"
#define PNLite_kKeyActiveDurationSinceLastCrash "activeDurationSinceLastCrash"
#define PNLite_kKeyBackgroundDurationSinceLastCrash                               \
    "backgroundDurationSinceLastCrash"
#define PNLite_kKeyLaunchesSinceLastCrash "launchesSinceLastCrash"
#define PNLite_kKeySessionsSinceLastCrash "sessionsSinceLastCrash"
#define PNLite_kKeySessionsSinceLaunch "sessionsSinceLaunch"

// ============================================================================
#pragma mark - Globals -
// ============================================================================

/** Location where stat file is stored. */
static const char *pnlite_g_stateFilePath;

/** Current state. */
static PNLite_KSCrash_State *pnlite_g_state;

// Avoiding static functions due to linker issues.

// ============================================================================
#pragma mark - JSON Encoding -
// ============================================================================

int pnlite_kscrashstate_i_onBooleanElement(const char *const name,
                                        const bool value,
                                        void *const userData) {
    PNLite_KSCrash_State *state = userData;

    if (strcmp(name, PNLite_kKeyCrashedLastLaunch) == 0) {
        state->crashedLastLaunch = value;
    }

    return PNLite_KSJSON_OK;
}

int pnlite_kscrashstate_i_onFloatingPointElement(const char *const name,
                                              const double value,
                                              void *const userData) {
    PNLite_KSCrash_State *state = userData;

    if (strcmp(name, PNLite_kKeyActiveDurationSinceLastCrash) == 0) {
        state->activeDurationSinceLastCrash = value;
    }
    if (strcmp(name, PNLite_kKeyBackgroundDurationSinceLastCrash) == 0) {
        state->backgroundDurationSinceLastCrash = value;
    }

    return PNLite_KSJSON_OK;
}

int pnlite_kscrashstate_i_onIntegerElement(const char *const name,
                                        const long long value,
                                        void *const userData) {
    PNLite_KSCrash_State *state = userData;

    if (strcmp(name, PNLite_kKeyFormatVersion) == 0) {
        if (value != PNLite_kFormatVersion) {
            PNLite_KSLOG_ERROR("Expected version 1 but got %lld", value);
            return PNLite_KSJSON_ERROR_INVALID_DATA;
        }
    } else if (strcmp(name, PNLite_kKeyLaunchesSinceLastCrash) == 0) {
        state->launchesSinceLastCrash = (int)value;
    } else if (strcmp(name, PNLite_kKeySessionsSinceLastCrash) == 0) {
        state->sessionsSinceLastCrash = (int)value;
    }

    // FP value might have been written as a whole number.
    return pnlite_kscrashstate_i_onFloatingPointElement(name, value, userData);
}

int pnlite_kscrashstate_i_onNullElement(__unused const char *const name,
                                     __unused void *const userData) {
    return PNLite_KSJSON_OK;
}

int pnlite_kscrashstate_i_onStringElement(__unused const char *const name,
                                       __unused const char *const value,
                                       __unused void *const userData) {
    return PNLite_KSJSON_OK;
}

int pnlite_kscrashstate_i_onBeginObject(__unused const char *const name,
                                     __unused void *const userData) {
    return PNLite_KSJSON_OK;
}

int pnlite_kscrashstate_i_onBeginArray(__unused const char *const name,
                                    __unused void *const userData) {
    return PNLite_KSJSON_OK;
}

int pnlite_kscrashstate_i_onEndContainer(__unused void *const userData) {
    return PNLite_KSJSON_OK;
}

int pnlite_kscrashstate_i_onEndData(__unused void *const userData) {
    return PNLite_KSJSON_OK;
}

/** Callback for adding JSON data.
 */
int pnlite_kscrashstate_i_addJSONData(const char *const data, const size_t length,
                                   void *const userData) {
    const int fd = *((int *)userData);
    const bool success = pnlite_ksfuwriteBytesToFD(fd, data, (ssize_t)length);
    return success ? PNLite_KSJSON_OK : PNLite_KSJSON_ERROR_CANNOT_ADD_DATA;
}

// ============================================================================
#pragma mark - Utility -
// ============================================================================

/** Load the persistent state portion of a crash context.
 *
 * @param context The context to load into.
 *
 * @param path The path to the file to read.
 *
 * @return true if the operation was successful.
 */
bool pnlite_kscrashstate_i_loadState(PNLite_KSCrash_State *const context,
                                  const char *const path) {
    // Stop if the file doesn't exist.
    // This is expected on the first run of the app.
    const int fd = open(path, O_RDONLY);
    if (fd < 0) {
        return false;
    }
    close(fd);

    char *data;
    size_t length;
    if (!pnlite_ksfureadEntireFile(path, &data, &length)) {
        PNLite_KSLOG_ERROR("%s: Could not load file", path);
        return false;
    }

    PNLite_KSJSONDecodeCallbacks callbacks;
    callbacks.onBeginArray = pnlite_kscrashstate_i_onBeginArray;
    callbacks.onBeginObject = pnlite_kscrashstate_i_onBeginObject;
    callbacks.onBooleanElement = pnlite_kscrashstate_i_onBooleanElement;
    callbacks.onEndContainer = pnlite_kscrashstate_i_onEndContainer;
    callbacks.onEndData = pnlite_kscrashstate_i_onEndData;
    callbacks.onFloatingPointElement =
        pnlite_kscrashstate_i_onFloatingPointElement;
    callbacks.onIntegerElement = pnlite_kscrashstate_i_onIntegerElement;
    callbacks.onNullElement = pnlite_kscrashstate_i_onNullElement;
    callbacks.onStringElement = pnlite_kscrashstate_i_onStringElement;

    size_t errorOffset = 0;

    const int result =
        pnlite_ksjsondecode(data, length, &callbacks, context, &errorOffset);
    free(data);
    if (result != PNLite_KSJSON_OK) {
        PNLite_KSLOG_ERROR("%s, offset %d: %s", path, errorOffset,
                        pnlite_ksjsonstringForError(result));
        return false;
    }
    return true;
}

/** Save the persistent state portion of a crash context.
 *
 * @param state The context to save from.
 *
 * @param path The path to the file to create.
 *
 * @return true if the operation was successful.
 */
bool pnlite_kscrashstate_i_saveState(const PNLite_KSCrash_State *const state,
                                  const char *const path) {
    int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        PNLite_KSLOG_ERROR("Could not open file %s for writing: %s", path,
                        strerror(errno));
        return false;
    }

    PNLite_KSJSONEncodeContext JSONContext;
    pnlite_ksjsonbeginEncode(&JSONContext, true, pnlite_kscrashstate_i_addJSONData,
                          &fd);

    int result;
    if ((result = pnlite_ksjsonbeginObject(&JSONContext, NULL)) != PNLite_KSJSON_OK) {
        goto done;
    }
    if ((result = pnlite_ksjsonaddIntegerElement(
             &JSONContext, PNLite_kKeyFormatVersion, PNLite_kFormatVersion)) !=
        PNLite_KSJSON_OK) {
        goto done;
    }
    // Record this launch crashed state into "crashed last launch" field.
    if ((result = pnlite_ksjsonaddBooleanElement(
             &JSONContext, PNLite_kKeyCrashedLastLaunch,
             state->crashedThisLaunch)) != PNLite_KSJSON_OK) {
        goto done;
    }
    if ((result = pnlite_ksjsonaddFloatingPointElement(
             &JSONContext, PNLite_kKeyActiveDurationSinceLastCrash,
             state->activeDurationSinceLastCrash)) != PNLite_KSJSON_OK) {
        goto done;
    }
    if ((result = pnlite_ksjsonaddFloatingPointElement(
             &JSONContext, PNLite_kKeyBackgroundDurationSinceLastCrash,
             state->backgroundDurationSinceLastCrash)) != PNLite_KSJSON_OK) {
        goto done;
    }
    if ((result = pnlite_ksjsonaddIntegerElement(
             &JSONContext, PNLite_kKeyLaunchesSinceLastCrash,
             state->launchesSinceLastCrash)) != PNLite_KSJSON_OK) {
        goto done;
    }
    if ((result = pnlite_ksjsonaddIntegerElement(
             &JSONContext, PNLite_kKeySessionsSinceLastCrash,
             state->sessionsSinceLastCrash)) != PNLite_KSJSON_OK) {
        goto done;
    }
    result = pnlite_ksjsonendEncode(&JSONContext);
    if (!pnlite_ksfuflushWriteBuffer(fd)) {
        PNLite_KSLOG_ERROR("Failed to flush write buffer");
    }

done:
    close(fd);

    if (result != PNLite_KSJSON_OK) {
        PNLite_KSLOG_ERROR("%s: %s", path, pnlite_ksjsonstringForError(result));
        return false;
    }
    return true;
}

// ============================================================================
#pragma mark - API -
// ============================================================================

bool pnlite_kscrashstate_init(const char *const stateFilePath,
                           PNLite_KSCrash_State *const state) {
    pnlite_g_stateFilePath = stateFilePath;
    pnlite_g_state = state;

    pnlite_kscrashstate_i_loadState(state, stateFilePath);

    state->sessionsSinceLaunch = 1;
    state->activeDurationSinceLaunch = 0;
    state->backgroundDurationSinceLaunch = 0;
    if (state->crashedLastLaunch) {
        state->activeDurationSinceLastCrash = 0;
        state->backgroundDurationSinceLastCrash = 0;
        state->launchesSinceLastCrash = 0;
        state->sessionsSinceLastCrash = 0;
    }
    state->crashedThisLaunch = false;

    // Simulate first transition to foreground
    state->launchesSinceLastCrash++;
    state->sessionsSinceLastCrash++;
    state->applicationIsInForeground = true;

    return pnlite_kscrashstate_i_saveState(state, stateFilePath);
}

void pnlite_kscrashstate_notifyAppActive(const bool isActive) {
    PNLite_KSCrash_State *const state = pnlite_g_state;

    state->applicationIsActive = isActive;
    if (isActive) {
        state->appStateTransitionTime = mach_absolute_time();
    } else {
        double duration = pnlite_ksmachtimeDifferenceInSeconds(
            mach_absolute_time(), state->appStateTransitionTime);
        state->activeDurationSinceLaunch += duration;
        state->activeDurationSinceLastCrash += duration;
    }
}

void pnlite_kscrashstate_notifyAppInForeground(const bool isInForeground) {
    PNLite_KSCrash_State *const state = pnlite_g_state;
    const char *const stateFilePath = pnlite_g_stateFilePath;

    state->applicationIsInForeground = isInForeground;
    if (isInForeground) {
        double duration = pnlite_ksmachtimeDifferenceInSeconds(
            mach_absolute_time(), state->appStateTransitionTime);
        state->backgroundDurationSinceLaunch += duration;
        state->backgroundDurationSinceLastCrash += duration;
        state->sessionsSinceLastCrash++;
        state->sessionsSinceLaunch++;
    } else {
        state->appStateTransitionTime = mach_absolute_time();
        pnlite_kscrashstate_i_saveState(state, stateFilePath);
    }
}

void pnlite_kscrashstate_notifyAppTerminate(void) {
    PNLite_KSCrash_State *const state = pnlite_g_state;
    const char *const stateFilePath = pnlite_g_stateFilePath;

    const double duration = pnlite_ksmachtimeDifferenceInSeconds(
        mach_absolute_time(), state->appStateTransitionTime);
    state->backgroundDurationSinceLastCrash += duration;
    pnlite_kscrashstate_i_saveState(state, stateFilePath);
}

void pnlite_kscrashstate_notifyAppCrash(void) {
    PNLite_KSCrash_State *const state = pnlite_g_state;
    const char *const stateFilePath = pnlite_g_stateFilePath;

    const double duration = pnlite_ksmachtimeDifferenceInSeconds(
        mach_absolute_time(), state->appStateTransitionTime);
    if (state->applicationIsActive) {
        state->activeDurationSinceLaunch += duration;
        state->activeDurationSinceLastCrash += duration;
    } else if (!state->applicationIsInForeground) {
        state->backgroundDurationSinceLaunch += duration;
        state->backgroundDurationSinceLastCrash += duration;
    }
    state->crashedThisLaunch = true;
    pnlite_kscrashstate_i_saveState(state, stateFilePath);
}

const PNLite_KSCrash_State *const pnlite_kscrashstate_currentState(void) {
    return pnlite_g_state;
}
