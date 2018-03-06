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

#import "PNLiteBannerPresenterFactory.h"
#import "PNLiteAssetGroupType.h"
#import "PNLiteBannerPresenterDecorator.h"
#import "PNLiteMRAIDBannerPresenter.h"
#import "PNLiteAdTracker.h"

@implementation PNLiteBannerPresenterFactory

- (PNLiteBannerPresenter *)createBannerPresenterWithAd:(PNLiteAd *)ad
                                          withDelegate:(NSObject<PNLiteBannerPresenterDelegate> *)delegate
{
    PNLiteBannerPresenter *bannerPresenter = [self createBannerPresenterFromAd:ad];
    if (!bannerPresenter) {
        return nil;
    }
    PNLiteBannerPresenterDecorator *bannerPresenterDecorator = [[PNLiteBannerPresenterDecorator alloc] initWithBannerPresenter:bannerPresenter
                                                                                                                 withAdTracker:[[PNLiteAdTracker alloc] initWithImpressionURLs:[ad beaconsDataWithType:kPNLiteAdTrackerImpression] withClickURLs:[ad beaconsDataWithType:kPNLiteAdTrackerClick]]
                                                                                                                  withDelegate:delegate];
    bannerPresenter.delegate = bannerPresenterDecorator;
    return bannerPresenterDecorator;
}

- (PNLiteBannerPresenter *)createBannerPresenterFromAd:(PNLiteAd *)ad
{
    switch (ad.assetGroupID.integerValue) {
        case MRAID_BANNER_1:
        case MRAID_BANNER_2: {
            PNLiteMRAIDBannerPresenter *mraidBannerPresenter = [[PNLiteMRAIDBannerPresenter alloc] initWithAd:ad];
            return mraidBannerPresenter;
            break;
        }
        default:
            NSLog(@"PNLiteBannerPresenterFactory - Asset Group %@ is an incompatible Asset Group ID for banner ad format", ad.assetGroupID);
            return nil;
            break;
    }
}

@end
