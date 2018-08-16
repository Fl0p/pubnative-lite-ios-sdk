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

#import "PNLiteDemoPNLiteNativeAdViewController.h"
#import <PubnativeLite/PubnativeLite.h>
#import "PNLiteDemoSettings.h"

@interface PNLiteDemoPNLiteNativeAdViewController () <PNLiteNativeAdLoaderDelegate, PNLiteNativeAdDelegate, PNLiteNativeAdFetchDelegate>

@property (weak, nonatomic) IBOutlet UIView *nativeAdContainer;
@property (weak, nonatomic) IBOutlet UIView *nativeAdContentInfo;
@property (weak, nonatomic) IBOutlet UIImageView *nativeAdIcon;
@property (weak, nonatomic) IBOutlet UILabel *nativeAdTitle;
@property (weak, nonatomic) IBOutlet PNLiteStarRatingView *nativeAdRating;
@property (weak, nonatomic) IBOutlet UIView *nativeAdBanner;
@property (weak, nonatomic) IBOutlet UILabel *nativeAdBody;
@property (weak, nonatomic) IBOutlet UIButton *nativeCallToAction;
@property (weak, nonatomic) IBOutlet UIActivityIndicatorView *nativeAdLoaderIndicator;
@property (nonatomic, strong) PNLiteNativeAdLoader *nativeAdLoader;
@property (nonatomic, strong) PNLiteNativeAd *nativeAd;
@end

@implementation PNLiteDemoPNLiteNativeAdViewController

- (void)dealloc
{
    self.nativeAdLoader = nil;
    [self.nativeAd stopTracking];
    self.nativeAd = nil;
}

- (void)viewDidLoad
{
    [super viewDidLoad];
    
    self.navigationItem.title = @"PubNative Lite Native Ad";
    [self.nativeAdLoaderIndicator stopAnimating];
}

- (IBAction)requestNativeAdTouchUpInside:(id)sender
{
    self.nativeAdContainer.hidden = YES;
    [self.nativeAdLoaderIndicator startAnimating];
    self.nativeAdLoader = [[PNLiteNativeAdLoader alloc] init];
    [self.nativeAdLoader loadNativeAdWithDelegate:self withZoneID:[PNLiteDemoSettings sharedInstance].zoneID];
}

#pragma mark - PNLiteNativeAdLoaderDelegate

- (void)nativeLoaderDidLoadWithNativeAd:(PNLiteNativeAd *)nativeAd
{
    NSLog(@"Native Ad: %@ did load",nativeAd);
    
    self.nativeAd = nativeAd;
    [self.nativeAd fetchNativeAdAssetsWithDelegate:self];
}

- (void)nativeLoaderDidFailWithError:(NSError *)error
{
    NSLog(@"Native Ad did fail with error: %@",error.localizedDescription);
    [self.nativeAdLoaderIndicator stopAnimating];
    UIAlertController *alertController = [UIAlertController
                                          alertControllerWithTitle:@"I have a bad feeling about this... 🙄"
                                          message:error.localizedDescription
                                          preferredStyle:UIAlertControllerStyleAlert];
    
    UIAlertAction * dismissAction = [UIAlertAction actionWithTitle:@"Dismiss" style:UIAlertActionStyleCancel handler:nil];
    UIAlertAction *retryAction = [UIAlertAction actionWithTitle:@"Retry" style:UIAlertActionStyleDefault handler:^(UIAlertAction * _Nonnull action) {
        [self requestNativeAdTouchUpInside:nil];
    }];
    [alertController addAction:dismissAction];
    [alertController addAction:retryAction];
    [self presentViewController:alertController animated:YES completion:nil];
}

#pragma mark - PNLiteNativeAdFetchDelegate

- (void)nativeAdDidFinishFetching:(PNLiteNativeAd *)nativeAd
{
    PNLiteNativeAdRenderer *renderer = [[PNLiteNativeAdRenderer alloc] init];
    renderer.contentInfoView = self.nativeAdContentInfo;
    renderer.iconView = self.nativeAdIcon;
    renderer.titleView = self.nativeAdTitle;
    renderer.starRatingView = self.nativeAdRating;
    renderer.bannerView = self.nativeAdBanner;
    renderer.bodyView = self.nativeAdBody;
    renderer.callToActionView = self.nativeCallToAction;
    
    [self.nativeAd renderAd:renderer];
    self.nativeAdContainer.hidden = NO;
    [self.nativeAd startTrackingView:self.nativeAdContainer withDelegate:self];
    [self.nativeAdLoaderIndicator stopAnimating];
}

- (void)nativeAd:(PNLiteNativeAd *)nativeAd didFailFetchingWithError:(NSError *)error
{
    NSLog(@"Native Ad did fail with error: %@",error.localizedDescription);
    [self.nativeAdLoaderIndicator stopAnimating];
    UIAlertController *alertController = [UIAlertController
                                          alertControllerWithTitle:@"I have a bad feeling about this... 🙄"
                                          message:error.localizedDescription
                                          preferredStyle:UIAlertControllerStyleAlert];
    
    UIAlertAction * dismissAction = [UIAlertAction actionWithTitle:@"Dismiss" style:UIAlertActionStyleCancel handler:nil];
    UIAlertAction *retryAction = [UIAlertAction actionWithTitle:@"Retry" style:UIAlertActionStyleDefault handler:^(UIAlertAction * _Nonnull action) {
        [self requestNativeAdTouchUpInside:nil];
    }];
    [alertController addAction:dismissAction];
    [alertController addAction:retryAction];
    [self presentViewController:alertController animated:YES completion:nil];
}

#pragma mark - PNLiteNativeAdDelegate

- (void)nativeAd:(PNLiteNativeAd *)nativeAd impressionConfirmedWithView:(UIView *)view
{
    NSLog(@"Native Ad did track impression:");
}

- (void)nativeAdDidClick:(PNLiteNativeAd *)nativeAd
{
    NSLog(@"Native Ad did track click:");
}

@end