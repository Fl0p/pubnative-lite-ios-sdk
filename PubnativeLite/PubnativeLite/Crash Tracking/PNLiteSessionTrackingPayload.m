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

#import "PNLiteSessionTrackingPayload.h"
#import "PNLiteCollections.h"
#import "PNLiteNotifier.h"
#import "PNLiteCrashTracker.h"
#import "PNLiteKeys.h"
#import "PNLite_KSSystemInfo.h"
#import "PNLiteKSCrashSysInfoParser.h"

@interface PNLiteCrashTracker ()
+ (PNLiteNotifier *)notifier;
@end

@implementation PNLiteSessionTrackingPayload

- (instancetype)initWithSessions:(NSArray<PNLiteSession *> *)sessions {
    if (self = [super init]) {
        _sessions = sessions;
    }
    return self;
}


- (NSDictionary *)toJson {
    
    NSMutableDictionary *dict = [NSMutableDictionary new];
    NSMutableArray *sessionData = [NSMutableArray new];
    
    for (PNLiteSession *session in self.sessions) {
        [sessionData addObject:[session toJson]];
    }
    PNLiteDictInsertIfNotNil(dict, sessionData, @"sessions");
    PNLiteDictSetSafeObject(dict, [PNLiteCrashTracker notifier].details, PNLiteKeyNotifier);
    
    NSDictionary *systemInfo = [PNLite_KSSystemInfo systemInfo];
    PNLiteDictSetSafeObject(dict, PNLiteParseAppState(systemInfo), @"app");
    PNLiteDictSetSafeObject(dict, PNLiteParseDeviceState(systemInfo), @"device");
    return [NSDictionary dictionaryWithDictionary:dict];
}

@end
