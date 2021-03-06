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

#import "HyBidInterstitialPresenterFactory.h"
#import "PNLiteAssetGroupType.h"
#import "PNLiteInterstitialPresenterDecorator.h"
#import "PNLiteMRAIDInterstitialPresenter.h"
#import "PNLiteVASTInterstitialPresenter.h"
#import "HyBidAdTracker.h"
#import "HyBidLogger.h"

@implementation HyBidInterstitialPresenterFactory

- (HyBidInterstitialPresenter *)createInterstitalPresenterWithAd:(HyBidAd *)ad
                                                    withDelegate:(NSObject<HyBidInterstitialPresenterDelegate> *)delegate {
    HyBidInterstitialPresenter *interstitialPresenter = [self createInterstitalPresenterFromAd:ad];
    if (!interstitialPresenter) {
        return nil;
    }
    PNLiteInterstitialPresenterDecorator *interstitialPresenterDecorator = [[PNLiteInterstitialPresenterDecorator alloc] initWithInterstitialPresenter:interstitialPresenter
                                                                                                                                         withAdTracker:[[HyBidAdTracker alloc] initWithImpressionURLs:[ad beaconsDataWithType:PNLiteAdTrackerImpression] withClickURLs:[ad beaconsDataWithType:PNLiteAdTrackerClick]]
                                                                                                                                          withDelegate:delegate];
    interstitialPresenter.delegate = interstitialPresenterDecorator;
    return interstitialPresenterDecorator;
}

- (HyBidInterstitialPresenter *)createInterstitalPresenterFromAd:(HyBidAd *)ad {
    switch (ad.assetGroupID.integerValue) {
        case MRAID_INTERSTITIAL:
        case MRAID_INTERSTITIAL_TABLET_1:
        case MRAID_INTERSTITIAL_TABLET_2: {
            PNLiteMRAIDInterstitialPresenter *mraidInterstitalPresenter = [[PNLiteMRAIDInterstitialPresenter alloc] initWithAd:ad];
            return mraidInterstitalPresenter;
            break;
        }
        case VAST_INTERSTITIAL_1:
        case VAST_INTERSTITIAL_2:
        case VAST_INTERSTITIAL_3:
        case VAST_INTERSTITIAL_4: {
            PNLiteVASTInterstitialPresenter *vastInterstitalPresenter = [[PNLiteVASTInterstitialPresenter alloc] initWithAd:ad];
            return vastInterstitalPresenter;
        }
        default:
            [HyBidLogger warningLogFromClass:NSStringFromClass([self class]) fromMethod:NSStringFromSelector(_cmd) withMessage:[NSString stringWithFormat:@"Asset Group %@ is an incompatible Asset Group ID for Interstitial ad format.", ad.assetGroupID]];
            return nil;
            break;
    }
}

@end
