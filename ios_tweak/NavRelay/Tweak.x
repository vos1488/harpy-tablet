/*
 * NavRelay — Jailbreak Tweak for relaying turn-by-turn navigation
 *            from iPhone to HARPY Remote (ESP32) via custom BLE GATT.
 *
 * Hooks Apple Maps, Google Maps, and Yandex Navigator to capture
 * navigation maneuver changes and write them to the ESP32's
 * custom BLE Nav Service characteristic.
 *
 * Service UUID:  E6A30000-B5A3-F393-E0A9-E50E24DCCA9E
 * Nav Data UUID: E6A30001-B5A3-F393-E0A9-E50E24DCCA9E
 *
 * Data format: "DIR|DIST|INSTRUCTION|STREET|ETA|SPEED|APP"
 *
 * Build with Theos:
 *   export THEOS=~/theos
 *   make package install
 *
 * Requirements:
 *   - Jailbroken iOS 14-17
 *   - Theos build system
 *   - Device or rootful/rootless jailbreak
 */

#import <Foundation/Foundation.h>
#import <CoreBluetooth/CoreBluetooth.h>
#import <CoreLocation/CoreLocation.h>
#import <UIKit/UIKit.h>
#import <objc/runtime.h>

/* ==================== Forward Declarations for Hooked Classes ==================== */

@interface MNStepCardViewController : UIViewController
- (id)currentStep;
- (id)maneuver;
@end

@interface MNNavigationViewController : UIViewController
@end

@interface GMSNavigationBannerView : UIView
@end

@interface YMKTurnInstructionView : UIView
@end

@interface MNStepCardViewController (NavRelay)
- (NSString *)findLabelText:(UIView *)view tag:(NSInteger)depth;
@end

/* ==================== Constants ==================== */

static NSString *const kESP32Name = @"HARPY Remote";

/* Custom Nav Service UUIDs (must match ESP32 ble_nav_service.c) */
static NSString *const kNavServiceUUID = @"E6A30000-B5A3-F393-E0A9-E50E24DCCA9E";
static NSString *const kNavDataUUID    = @"E6A30001-B5A3-F393-E0A9-E50E24DCCA9E";

/* Direction codes matching ble_nav_direction_t enum on ESP32 */
typedef NS_ENUM(NSInteger, NavDirection) {
    NavDirectionNone       = 0,
    NavDirectionStraight   = 1,
    NavDirectionLeft       = 2,
    NavDirectionRight      = 3,
    NavDirectionSlightLeft = 4,
    NavDirectionSlightRight= 5,
    NavDirectionUTurn      = 6,
    NavDirectionArrive     = 7,
    NavDirectionRoundabout = 8,
};

/* ==================== BLE Manager ==================== */

@interface NavRelayBLE : NSObject <CBCentralManagerDelegate, CBPeripheralDelegate>
@property (nonatomic, strong) CBCentralManager *centralManager;
@property (nonatomic, strong) CBPeripheral *connectedPeripheral;
@property (nonatomic, strong) CBCharacteristic *navCharacteristic;
@property (nonatomic, assign) BOOL isReady;
@property (nonatomic, strong) NSString *pendingData;
+ (instancetype)shared;
- (void)sendNavData:(NSString *)data;
- (void)sendNavEnd;
@end

@implementation NavRelayBLE

+ (instancetype)shared {
    static NavRelayBLE *instance = nil;
    static dispatch_once_t onceToken;
    dispatch_once(&onceToken, ^{
        instance = [[NavRelayBLE alloc] init];
    });
    return instance;
}

- (instancetype)init {
    self = [super init];
    if (self) {
        _isReady = NO;
        /* Delay BLE init — don't create CBCentralManager in constructor,
         * wait until app is fully loaded to avoid UIBackgroundModes crash */
        NSLog(@"[NavRelay] BLE Manager created (lazy init)");
    }
    return self;
}

- (void)ensureBLE {
    if (self.centralManager) return;
    /* Do NOT use CBCentralManagerOptionRestoreIdentifierKey — nav apps
     * don't have bluetooth-central in UIBackgroundModes, causes crash */
    self.centralManager = [[CBCentralManager alloc]
        initWithDelegate:self
        queue:dispatch_get_global_queue(DISPATCH_QUEUE_PRIORITY_DEFAULT, 0)
        options:nil];
    NSLog(@"[NavRelay] CBCentralManager initialized");
}

#pragma mark - Public API

- (void)sendNavData:(NSString *)data {
    [self ensureBLE];
    if (self.isReady && self.navCharacteristic) {
        NSData *bytes = [data dataUsingEncoding:NSUTF8StringEncoding];
        if (bytes.length > 512) {
            bytes = [bytes subdataWithRange:NSMakeRange(0, 512)];
        }
        [self.connectedPeripheral writeValue:bytes
                           forCharacteristic:self.navCharacteristic
                                        type:CBCharacteristicWriteWithResponse];
        NSLog(@"[NavRelay] Sent: %@", data);
    } else {
        NSLog(@"[NavRelay] Not ready, queueing: %@", data);
        self.pendingData = data;
        [self tryConnect];
    }
}

- (void)sendNavEnd {
    [self sendNavData:@"NAV_END"];
}

#pragma mark - Connection

- (void)tryConnect {
    [self ensureBLE];
    if (self.connectedPeripheral && self.connectedPeripheral.state == CBPeripheralStateConnected) {
        return; /* Already connected */
    }
    if (self.centralManager.state != CBManagerStatePoweredOn) {
        NSLog(@"[NavRelay] BLE not powered on yet, will retry on state change");
        return;
    }

    /* Try to retrieve already-connected peripherals (iPhone connects for HID) */
    CBUUID *hidSvcUUID = [CBUUID UUIDWithString:@"1812"];

    NSArray *connected = [self.centralManager retrieveConnectedPeripheralsWithServices:@[hidSvcUUID]];
    for (CBPeripheral *p in connected) {
        if ([p.name containsString:@"HARPY"]) {
            NSLog(@"[NavRelay] Found connected HARPY: %@", p.name);
            self.connectedPeripheral = p;
            p.delegate = self;
            [self.centralManager connectPeripheral:p options:nil];
            return;
        }
    }

    /* If not found via HID, try scanning */
    NSLog(@"[NavRelay] HARPY not found in connected peripherals, scanning...");
    [self.centralManager scanForPeripheralsWithServices:nil options:nil];

    /* Stop scan after 10 seconds */
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 10 * NSEC_PER_SEC),
                   dispatch_get_global_queue(0, 0), ^{
        [self.centralManager stopScan];
    });
}

#pragma mark - CBCentralManagerDelegate

- (void)centralManagerDidUpdateState:(CBCentralManager *)central {
    NSLog(@"[NavRelay] BLE state: %ld", (long)central.state);
    if (central.state == CBManagerStatePoweredOn) {
        [self tryConnect];
    }
}

- (void)centralManager:(CBCentralManager *)central
    didDiscoverPeripheral:(CBPeripheral *)peripheral
    advertisementData:(NSDictionary<NSString *,id> *)advertisementData
    RSSI:(NSNumber *)RSSI {
    NSString *name = peripheral.name ?: advertisementData[CBAdvertisementDataLocalNameKey];
    if ([name containsString:@"HARPY"]) {
        NSLog(@"[NavRelay] Discovered HARPY: %@ RSSI=%@", name, RSSI);
        [central stopScan];
        self.connectedPeripheral = peripheral;
        peripheral.delegate = self;
        [central connectPeripheral:peripheral options:nil];
    }
}

- (void)centralManager:(CBCentralManager *)central
    didConnectPeripheral:(CBPeripheral *)peripheral {
    NSLog(@"[NavRelay] Connected to %@", peripheral.name);
    CBUUID *navSvcUUID = [CBUUID UUIDWithString:kNavServiceUUID];
    [peripheral discoverServices:@[navSvcUUID]];
}

- (void)centralManager:(CBCentralManager *)central
    didDisconnectPeripheral:(CBPeripheral *)peripheral
    error:(NSError *)error {
    NSLog(@"[NavRelay] Disconnected: %@", error);
    self.isReady = NO;
    self.navCharacteristic = nil;

    /* Auto-reconnect after 2 seconds */
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 2 * NSEC_PER_SEC),
                   dispatch_get_global_queue(0, 0), ^{
        [self tryConnect];
    });
}

#pragma mark - CBPeripheralDelegate

- (void)peripheral:(CBPeripheral *)peripheral
    didDiscoverServices:(NSError *)error {
    if (error) {
        NSLog(@"[NavRelay] Service discovery error: %@", error);
        return;
    }
    CBUUID *navSvcUUID = [CBUUID UUIDWithString:kNavServiceUUID];
    for (CBService *svc in peripheral.services) {
        NSLog(@"[NavRelay] Found service: %@", svc.UUID);
        if ([svc.UUID isEqual:navSvcUUID]) {
            CBUUID *navDataUUID = [CBUUID UUIDWithString:kNavDataUUID];
            [peripheral discoverCharacteristics:@[navDataUUID] forService:svc];
        }
    }
}

- (void)peripheral:(CBPeripheral *)peripheral
    didDiscoverCharacteristicsForService:(CBService *)service
    error:(NSError *)error {
    if (error) {
        NSLog(@"[NavRelay] Characteristic discovery error: %@", error);
        return;
    }
    CBUUID *navDataUUID = [CBUUID UUIDWithString:kNavDataUUID];
    for (CBCharacteristic *chr in service.characteristics) {
        NSLog(@"[NavRelay] Found char: %@", chr.UUID);
        if ([chr.UUID isEqual:navDataUUID]) {
            self.navCharacteristic = chr;
            self.isReady = YES;
            NSLog(@"[NavRelay] *** READY — Nav characteristic found ***");

            /* Send any pending data */
            if (self.pendingData) {
                [self sendNavData:self.pendingData];
                self.pendingData = nil;
            }
        }
    }
}

- (void)peripheral:(CBPeripheral *)peripheral
    didWriteValueForCharacteristic:(CBCharacteristic *)characteristic
    error:(NSError *)error {
    if (error) {
        NSLog(@"[NavRelay] Write error: %@", error);
    }
}

@end

/* ==================== Navigation Direction Parser ==================== */

static NavDirection parseDirection(NSString *text) {
    if (!text) return NavDirectionStraight;
    NSString *lower = [text lowercaseString];

    /* Russian keywords */
    if ([lower containsString:@"налево"] || [lower containsString:@"лево"])
        return NavDirectionLeft;
    if ([lower containsString:@"направо"] || [lower containsString:@"право"])
        return NavDirectionRight;
    if ([lower containsString:@"развернитесь"] || [lower containsString:@"разворот"])
        return NavDirectionUTurn;
    if ([lower containsString:@"прибыли"] || [lower containsString:@"место назначения"])
        return NavDirectionArrive;
    if ([lower containsString:@"круговое"] || [lower containsString:@"кольц"])
        return NavDirectionRoundabout;
    if ([lower containsString:@"прямо"])
        return NavDirectionStraight;
    if ([lower containsString:@"плавно налево"] || [lower containsString:@"левее"])
        return NavDirectionSlightLeft;
    if ([lower containsString:@"плавно направо"] || [lower containsString:@"правее"])
        return NavDirectionSlightRight;

    /* English keywords */
    if ([lower containsString:@"turn left"] || [lower containsString:@" left"])
        return NavDirectionLeft;
    if ([lower containsString:@"turn right"] || [lower containsString:@" right"])
        return NavDirectionRight;
    if ([lower containsString:@"u-turn"] || [lower containsString:@"u turn"])
        return NavDirectionUTurn;
    if ([lower containsString:@"arrive"] || [lower containsString:@"destination"])
        return NavDirectionArrive;
    if ([lower containsString:@"roundabout"])
        return NavDirectionRoundabout;
    if ([lower containsString:@"slight left"] || [lower containsString:@"bear left"])
        return NavDirectionSlightLeft;
    if ([lower containsString:@"slight right"] || [lower containsString:@"bear right"])
        return NavDirectionSlightRight;
    if ([lower containsString:@"straight"] || [lower containsString:@"continue"])
        return NavDirectionStraight;

    return NavDirectionStraight;
}

/* Helper to format and send nav data */
static void sendManeuver(NSString *instruction, NSString *distance,
                         NSString *street, NSString *eta,
                         NSString *speed, NSString *appName) {
    NavDirection dir = parseDirection(instruction);
    NSString *data = [NSString stringWithFormat:@"%ld|%@|%@|%@|%@|%@|%@",
                      (long)dir,
                      distance ?: @"",
                      instruction ?: @"",
                      street ?: @"",
                      eta ?: @"",
                      speed ?: @"",
                      appName ?: @"Maps"];
    [[NavRelayBLE shared] sendNavData:data];
}

/* ==================== Apple Maps Hooks ==================== */
/*
 * Apple Maps class names vary by iOS version.
 * Common patterns (iOS 15-17):
 *   - MNStepCardViewController — Turn card at top of screen
 *   - MNNavigationViewController — Main navigation VC
 *   - MNManeuver — Maneuver data model
 *   - MNStepInfo — Step info model
 *
 * For iOS 16-17, the step banner uses:
 *   - MNStepBannerViewController
 *   - MNStepCardContainerViewController
 *
 * Use `class-dump` on MapsSupport.framework and Maps.app
 * to find exact class names for your iOS version:
 *   class-dump /Applications/Maps.app/Maps > maps_headers.h
 *   class-dump /System/Library/PrivateFrameworks/MapsSupport.framework/MapsSupport > maps_support.h
 *   class-dump /System/Library/PrivateFrameworks/Navigation.framework/Navigation > navigation.h
 */

/* Track last instruction to avoid duplicates */
static NSString *lastInstruction = nil;

/* Throttle: minimum interval between hook processing (seconds) */
static CFAbsoluteTime lastHookTime = 0;
static const CFAbsoluteTime kHookThrottleInterval = 0.5; /* 500ms */

static BOOL shouldThrottle(void) {
    CFAbsoluteTime now = CFAbsoluteTimeGetCurrent();
    if (now - lastHookTime < kHookThrottleInterval) return YES;
    lastHookTime = now;
    return NO;
}

/*
 * Hook: MNStepCardViewController (iOS 15-17)
 * Called when the turn card updates its maneuver view.
 */
%hook MNStepCardViewController

- (void)viewDidLayoutSubviews {
    %orig;

    /* Throttle to avoid performance issues */
    if (shouldThrottle()) return;

    @try {
        /* Try to extract instruction from the step/maneuver model */
        NSString *instruction = nil;
        NSString *distance = nil;
        NSString *street = nil;

        /* Try common property names across iOS versions */
        if ([self respondsToSelector:@selector(currentStep)]) {
            id step = [self performSelector:@selector(currentStep)];
            if ([step respondsToSelector:@selector(instruction)])
                instruction = [step performSelector:@selector(instruction)];
            if ([step respondsToSelector:@selector(distanceString)])
                distance = [step performSelector:@selector(distanceString)];
            if ([step respondsToSelector:@selector(roadName)])
                street = [step performSelector:@selector(roadName)];
        }

        if ([self respondsToSelector:@selector(maneuver)]) {
            id maneuver = [self performSelector:@selector(maneuver)];
            if (!instruction && [maneuver respondsToSelector:@selector(instructionString)])
                instruction = [maneuver performSelector:@selector(instructionString)];
            if (!distance && [maneuver respondsToSelector:@selector(distanceRemainingString)])
                distance = [maneuver performSelector:@selector(distanceRemainingString)];
            if (!street && [maneuver respondsToSelector:@selector(name)])
                street = [maneuver performSelector:@selector(name)];
        }

        /* Fallback: scan view hierarchy for labels (max 6 levels deep) */
        if (!instruction) {
            instruction = [self findLabelText:self.view tag:6];
        }

        if (instruction && ![instruction isEqualToString:lastInstruction]) {
            lastInstruction = [instruction copy];
            sendManeuver(instruction, distance, street, nil, nil, @"Apple Maps");
        }
    } @catch (NSException *ex) {
        NSLog(@"[NavRelay] Apple Maps hook error: %@", ex);
    }
}

/* Recursive label finder */
%new
- (NSString *)findLabelText:(UIView *)view tag:(NSInteger)depth {
    if (depth <= 0) return nil;
    for (UIView *sub in view.subviews) {
        if ([sub isKindOfClass:[UILabel class]]) {
            UILabel *lbl = (UILabel *)sub;
            NSString *text = lbl.text;
            if (text.length > 3 && text.length < 200) {
                /* Heuristic: look for navigation-like text */
                NSString *lower = [text lowercaseString];
                if ([lower containsString:@"turn"] || [lower containsString:@"continue"] ||
                    [lower containsString:@"left"] || [lower containsString:@"right"] ||
                    [lower containsString:@"arrive"] || [lower containsString:@"head"] ||
                    [lower containsString:@"поверн"] || [lower containsString:@"прямо"] ||
                    [lower containsString:@"налево"] || [lower containsString:@"направо"]) {
                    return text;
                }
            }
        }
        NSString *found = [self findLabelText:sub tag:depth-1];
        if (found) return found;
    }
    return nil;
}

%end

/*
 * Hook: MNNavigationViewController — navigation session lifecycle
 */
%hook MNNavigationViewController

- (void)viewDidDisappear:(BOOL)animated {
    %orig;
    [[NavRelayBLE shared] sendNavEnd];
    lastInstruction = nil;
    NSLog(@"[NavRelay] Apple Maps navigation ended");
}

%end

/* ==================== Google Maps Hooks ==================== */
// Google Maps uses GMSNavigationBannerView, GMSNavInstruction, etc.
// Use class-dump on the Google Maps binary to find exact class names.

%hook GMSNavigationBannerView

- (void)layoutSubviews {
    %orig;

    if (shouldThrottle()) return;

    @try {
        NSString *instruction = nil;
        NSString *distance = nil;

        /* Scan subviews for navigation labels */
        for (UIView *sub in self.subviews) {
            for (UIView *sub2 in sub.subviews) {
                if ([sub2 isKindOfClass:[UILabel class]]) {
                    UILabel *lbl = (UILabel *)sub2;
                    NSString *text = lbl.text;
                    if (!text) continue;

                    /* Distance label (contains m, km, mi, ft, м, км) */
                    if (!distance) {
                        NSString *lower = [text lowercaseString];
                        if ([lower hasSuffix:@" m"] || [lower hasSuffix:@" km"] ||
                            [lower hasSuffix:@" mi"] || [lower hasSuffix:@" ft"] ||
                            [lower hasSuffix:@" м"] || [lower hasSuffix:@" км"]) {
                            distance = text;
                            continue;
                        }
                    }

                    /* Instruction label */
                    if (!instruction && text.length > 3) {
                        instruction = text;
                    }
                }
            }
        }

        if (instruction && ![instruction isEqualToString:lastInstruction]) {
            lastInstruction = [instruction copy];
            sendManeuver(instruction, distance, nil, nil, nil, @"Google Maps");
        }
    } @catch (NSException *ex) {
        NSLog(@"[NavRelay] Google Maps hook error: %@", ex);
    }
}

%end

/* ==================== Yandex Navigator / 2GIS Hooks ==================== */
/*
 * Yandex Navigator and 2GIS use UIKit views.
 * Hook their main navigation view controller.
 *
 * Yandex: YMKNavigationViewController, YMKTurnInstructionView
 * 2GIS: DGNavigationViewController
 */

%hook YMKTurnInstructionView

- (void)layoutSubviews {
    %orig;

    if (shouldThrottle()) return;

    @try {
        NSString *instruction = nil;
        NSString *distance = nil;

        for (UIView *sub in self.subviews) {
            if ([sub isKindOfClass:[UILabel class]]) {
                UILabel *lbl = (UILabel *)sub;
                NSString *text = lbl.text;
                if (!text || text.length < 2) continue;

                if (!distance) {
                    NSString *lower = [text lowercaseString];
                    if ([lower hasSuffix:@" м"] || [lower hasSuffix:@" км"] ||
                        [lower hasSuffix:@" m"] || [lower hasSuffix:@" km"]) {
                        distance = text;
                        continue;
                    }
                }
                if (!instruction && text.length > 3) {
                    instruction = text;
                }
            }
        }

        if (instruction && ![instruction isEqualToString:lastInstruction]) {
            lastInstruction = [instruction copy];
            sendManeuver(instruction, distance, nil, nil, nil, @"Yandex Navi");
        }
    } @catch (NSException *ex) {
        NSLog(@"[NavRelay] Yandex hook error: %@", ex);
    }
}

%end

/* ==================== Debug Logging (no UIViewController hook) ==================== */
/*
 * The universal UIViewController hook was removed because it caused
 * crashes — hooking viewDidLayoutSubviews on ALL VCs is too invasive.
 *
 * To find navigation-related class names for your iOS version, SSH
 * into the device and run:
 *   class-dump /Applications/Maps.app/Maps | grep -i 'step\|maneuver\|nav'
 *
 * Then add a targeted %hook for that specific class.
 */

/* ==================== Tweak Initialization ==================== */

%ctor {
    @autoreleasepool {
        NSString *bundleID = [[NSBundle mainBundle] bundleIdentifier];
        NSLog(@"[NavRelay] Tweak loaded in %@", bundleID);

        /* Only proceed if we're in a navigation app */
        if (!bundleID) return;
        NSArray *supportedApps = @[
            @"com.apple.Maps",
            @"com.google.Maps",
            @"ru.yandex.yandexnavi",
            @"ru.yandex.mobile.navigator",
            @"ru.dublgis.dgismobile",
            @"com.waze.iphone"
        ];
        BOOL supported = NO;
        for (NSString *app in supportedApps) {
            if ([bundleID isEqualToString:app]) { supported = YES; break; }
        }
        if (!supported) {
            NSLog(@"[NavRelay] Unsupported app %@, not loading", bundleID);
            return;
        }

        /* Create singleton (BLE init is lazy, won't happen until needed) */
        [NavRelayBLE shared];

        /* Delayed BLE init — wait for app to fully load */
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 3 * NSEC_PER_SEC),
                       dispatch_get_main_queue(), ^{
            [[NavRelayBLE shared] ensureBLE];
            NSLog(@"[NavRelay] BLE init triggered after app load");
        });
    }
}
