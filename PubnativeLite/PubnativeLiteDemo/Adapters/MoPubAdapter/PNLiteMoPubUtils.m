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

#import "PNLiteMoPubUtils.h"

NSString *const kPNLiteMoPubAdapterKeyZoneID = @"pn_zone_id";
NSString *const kPNLiteMoPubAdapterKeyAppToken = @"pn_app_token";

@implementation PNLiteMoPubUtils

+ (BOOL)isZoneIDValid:(NSDictionary *)extras
{
    return [PNLiteMoPubUtils zoneID:extras];
}

+ (BOOL)isAppTokenValid:(NSDictionary *)extras
{
    return [PNLiteMoPubUtils appToken:extras];
}

+ (BOOL)areExtrasValid:(NSDictionary *)extras
{
    return [PNLiteMoPubUtils zoneID:extras] && [PNLiteMoPubUtils appToken:extras];
}

+ (NSString *)zoneID:(NSDictionary *)extras
{
    return [PNLiteMoPubUtils valueWithKey:kPNLiteMoPubAdapterKeyZoneID fromExtras:extras];
}

+ (NSString *)appToken:(NSDictionary *)extras
{
    return [PNLiteMoPubUtils valueWithKey:kPNLiteMoPubAdapterKeyAppToken fromExtras:extras];
}

+ (NSString *)valueWithKey:(NSString *)key
                fromExtras:(NSDictionary *)extras {
    NSString *result = nil;
    if (extras && [extras objectForKey:key]) {
        NSString *param = [extras objectForKey:key];
        if ([param length] != 0) {
            result = param;
        }
    }
    return result;
}

@end
