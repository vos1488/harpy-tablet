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
 * v1.1.0 — Timer-based polling instead of layout hooks (crash fix)
 */

#import <Foundation/Foundation.h>
#import <CoreBluetooth/CoreBluetooth.h>
#import <UIKit/UIKit.h>
#import <objc/runtime.h>

/* ==================== Forward Declarations ==================== */

@interface MNStepCardViewController : UIViewController
@end

@interface MNNavigationViewController : UIViewController
@end

@interface GMSNavigationBannerView : UIView
@end

@interface YMKTurnInstructionView : UIView
@end

/* ==================== Constants ==================== */

static NSString *const kNavServiceUUID = @"E6A30000-B5A3-F393-E0A9-E50E24DCCA9E";
static NSString *const kNavDataUUID    = @"E6A30001-B5A3-F393-E0A9-E50E24DCCA9E";

/* Associated-object key for polling timers */
static char kNavTimerKey;

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
- (void)ensureBLE;
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
    }
    return self;
}

- (void)ensureBLE {
    if (self.centralManager) return;
    self.centralManager = [[CBCentralManager alloc]
        initWithDelegate:self
        queue:dispatch_get_global_queue(DISPATCH_QUEUE_PRIORITY_DEFAULT, 0)
        options:nil];
    NSLog(@"[NavRelay] CBCentralManager initialized");
}

#pragma mark - Public API

- (void)sendNavData:(NSString *)data {
    [self ensureBLE];
    if (self.isReady && self.navCharacteristic && self.connectedPeripheral) {
        NSData *bytes = [data dataUsingEncoding:NSUTF8StringEncoding];
        if (bytes.length > 512) {
            bytes = [bytes subdataWithRange:NSMakeRange(0, 512)];
        }
        @try {
            [self.connectedPeripheral writeValue:bytes
                               forCharacteristic:self.navCharacteristic
                                            type:CBCharacteristicWriteWithResponse];
        } @catch (NSException *ex) {
            NSLog(@"[NavRelay] Write exception: %@", ex);
            self.isReady = NO;
        }
    } else {
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
    if (self.connectedPeripheral &&
        self.connectedPeripheral.state == CBPeripheralStateConnected) {
        return;
    }
    if (self.centralManager.state != CBManagerStatePoweredOn) return;

    CBUUID *hidSvcUUID = [CBUUID UUIDWithString:@"1812"];
    NSArray *connected = [self.centralManager
        retrieveConnectedPeripheralsWithServices:@[hidSvcUUID]];
    for (CBPeripheral *p in connected) {
        if ([p.name containsString:@"HARPY"]) {
            NSLog(@"[NavRelay] Found connected HARPY: %@", p.name);
            self.connectedPeripheral = p;
            p.delegate = self;
            [self.centralManager connectPeripheral:p options:nil];
            return;
        }
    }

    NSLog(@"[NavRelay] Scanning for HARPY...");
    [self.centralManager scanForPeripheralsWithServices:nil options:nil];
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
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 2 * NSEC_PER_SEC),
                   dispatch_get_global_queue(0, 0), ^{
        [self tryConnect];
    });
}

#pragma mark - CBPeripheralDelegate

- (void)peripheral:(CBPeripheral *)peripheral
    didDiscoverServices:(NSError *)error {
    if (error) { NSLog(@"[NavRelay] Svc error: %@", error); return; }
    CBUUID *navSvcUUID = [CBUUID UUIDWithString:kNavServiceUUID];
    for (CBService *svc in peripheral.services) {
        if ([svc.UUID isEqual:navSvcUUID]) {
            CBUUID *navDataUUID = [CBUUID UUIDWithString:kNavDataUUID];
            [peripheral discoverCharacteristics:@[navDataUUID] forService:svc];
        }
    }
}

- (void)peripheral:(CBPeripheral *)peripheral
    didDiscoverCharacteristicsForService:(CBService *)service
    error:(NSError *)error {
    if (error) { NSLog(@"[NavRelay] Chr error: %@", error); return; }
    CBUUID *navDataUUID = [CBUUID UUIDWithString:kNavDataUUID];
    for (CBCharacteristic *chr in service.characteristics) {
        if ([chr.UUID isEqual:navDataUUID]) {
            self.navCharacteristic = chr;
            self.isReady = YES;
            NSLog(@"[NavRelay] *** READY ***");
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
    if (error) NSLog(@"[NavRelay] Write err: %@", error);
}

@end

/* ==================== Direction Parser ==================== */

static NavDirection parseDirection(NSString *text) {
    if (!text) return NavDirectionStraight;
    NSString *lower = [text lowercaseString];

    if ([lower containsString:@"плавно налево"] || [lower containsString:@"левее"] ||
        [lower containsString:@"slight left"] || [lower containsString:@"bear left"])
        return NavDirectionSlightLeft;
    if ([lower containsString:@"плавно направо"] || [lower containsString:@"правее"] ||
        [lower containsString:@"slight right"] || [lower containsString:@"bear right"])
        return NavDirectionSlightRight;
    if ([lower containsString:@"налево"] || [lower containsString:@"лево"] ||
        [lower containsString:@"turn left"] || [lower containsString:@" left"])
        return NavDirectionLeft;
    if ([lower containsString:@"направо"] || [lower containsString:@"право"] ||
        [lower containsString:@"turn right"] || [lower containsString:@" right"])
        return NavDirectionRight;
    if ([lower containsString:@"развернитесь"] || [lower containsString:@"разворот"] ||
        [lower containsString:@"u-turn"] || [lower containsString:@"u turn"])
        return NavDirectionUTurn;
    if ([lower containsString:@"прибыли"] || [lower containsString:@"место назначения"] ||
        [lower containsString:@"arrive"] || [lower containsString:@"destination"])
        return NavDirectionArrive;
    if ([lower containsString:@"круговое"] || [lower containsString:@"кольц"] ||
        [lower containsString:@"roundabout"])
        return NavDirectionRoundabout;
    if ([lower containsString:@"прямо"] || [lower containsString:@"straight"] ||
        [lower containsString:@"continue"])
        return NavDirectionStraight;

    return NavDirectionStraight;
}

/* Track last instruction to avoid BLE spam */
static NSString *s_lastInstruction = nil;

static void sendManeuver(NSString *instruction, NSString *distance,
                         NSString *street, NSString *appName) {
    if (!instruction || instruction.length == 0) return;
    /* De-duplicate */
    if (s_lastInstruction && [instruction isEqualToString:s_lastInstruction]) return;
    s_lastInstruction = [instruction copy];

    NavDirection dir = parseDirection(instruction);
    NSString *data = [NSString stringWithFormat:@"%ld|%@|%@|%@|||%@",
                      (long)dir,
                      distance ?: @"",
                      instruction ?: @"",
                      street ?: @"",
                      appName ?: @"Maps"];
    [[NavRelayBLE shared] sendNavData:data];
}

/* ==================== Safe View Scanner ==================== */
/*
 * Scan a SNAPSHOT of the view hierarchy to find UILabel texts.
 * Uses [view.subviews copy] to avoid mutation-during-enumeration crash.
 * Max depth 5. Returns first label matching nav keywords.
 */

static BOOL isNavText(NSString *text) {
    if (!text || text.length < 3 || text.length > 300) return NO;
    NSString *lower = [text lowercaseString];
    return ([lower containsString:@"turn"] || [lower containsString:@"continue"] ||
            [lower containsString:@"left"] || [lower containsString:@"right"] ||
            [lower containsString:@"arrive"] || [lower containsString:@"head"] ||
            [lower containsString:@"merge"] || [lower containsString:@"exit"] ||
            [lower containsString:@"поверн"] || [lower containsString:@"прямо"] ||
            [lower containsString:@"налево"] || [lower containsString:@"направо"] ||
            [lower containsString:@"развор"] || [lower containsString:@"съезд"]);
}

static BOOL isDistanceText(NSString *text) {
    if (!text || text.length < 2 || text.length > 30) return NO;
    NSString *lower = [text lowercaseString];
    return ([lower hasSuffix:@" m"] || [lower hasSuffix:@" km"] ||
            [lower hasSuffix:@" mi"] || [lower hasSuffix:@" ft"] ||
            [lower hasSuffix:@" м"] || [lower hasSuffix:@" км"] ||
            [lower hasSuffix:@" yd"]);
}

static NSString *findNavLabel(UIView *root, NSInteger depth) {
    if (!root || depth <= 0) return nil;
    @try {
        NSArray *subs = [root.subviews copy]; /* snapshot — mutation-safe */
        if (!subs) return nil;
        for (UIView *sub in subs) {
            if ([sub isKindOfClass:[UILabel class]]) {
                NSString *text = ((UILabel *)sub).text;
                if (isNavText(text)) return text;
            }
            NSString *found = findNavLabel(sub, depth - 1);
            if (found) return found;
        }
    } @catch (NSException *e) {
        /* Silently ignore — view hierarchy may be in flux */
    }
    return nil;
}

static NSString *findDistanceLabel(UIView *root, NSInteger depth) {
    if (!root || depth <= 0) return nil;
    @try {
        NSArray *subs = [root.subviews copy];
        if (!subs) return nil;
        for (UIView *sub in subs) {
            if ([sub isKindOfClass:[UILabel class]]) {
                NSString *text = ((UILabel *)sub).text;
                if (isDistanceText(text)) return text;
            }
            NSString *found = findDistanceLabel(sub, depth - 1);
            if (found) return found;
        }
    } @catch (NSException *e) { }
    return nil;
}

/* ==================== Timer Helpers ==================== */
/*
 * Start/stop a repeating NSTimer stored as an associated object.
 * Uses __weak capture to avoid retain cycles.
 * Timer fires every 1.5 seconds on the main run loop.
 */

static void startPollTimer(id owner, void (^pollBlock)(void)) {
    NSTimer *existing = objc_getAssociatedObject(owner, &kNavTimerKey);
    if (existing && [existing isValid]) return;

    __weak id weakOwner = owner;
    NSTimer *timer = [NSTimer scheduledTimerWithTimeInterval:1.5
                                                    repeats:YES
                                                      block:^(NSTimer *t) {
        if (!weakOwner) { [t invalidate]; return; }
        @try {
            pollBlock();
        } @catch (NSException *e) {
            NSLog(@"[NavRelay] Timer poll error: %@", e);
        }
    }];
    objc_setAssociatedObject(owner, &kNavTimerKey, timer,
                             OBJC_ASSOCIATION_RETAIN_NONATOMIC);
    NSLog(@"[NavRelay] Poll timer started for %@",
          NSStringFromClass([owner class]));
}

static void stopPollTimer(id owner) {
    NSTimer *timer = objc_getAssociatedObject(owner, &kNavTimerKey);
    if (timer) {
        [timer invalidate];
        objc_setAssociatedObject(owner, &kNavTimerKey, nil,
                                 OBJC_ASSOCIATION_RETAIN_NONATOMIC);
        NSLog(@"[NavRelay] Poll timer stopped for %@",
              NSStringFromClass([owner class]));
    }
}

/* ==================== Apple Maps Hooks ==================== */
/*
 * Timer-based: start on viewDidAppear, stop on viewWillDisappear/dealloc.
 * Polls view hierarchy safely every 1.5 seconds.
 * Also tries KVC (valueForKey:) for model-level data.
 */

%hook MNStepCardViewController

- (void)viewDidAppear:(BOOL)animated {
    %orig;

    __weak MNStepCardViewController *weakSelf = self;
    startPollTimer(self, ^{
        MNStepCardViewController *ss = weakSelf;
        if (!ss) return;

        NSString *instruction = nil;
        NSString *distance = nil;
        NSString *street = nil;

        /* Try KVC (safe under ARC, throws catchable NSException) */
        @try {
            id step = [ss valueForKey:@"currentStep"];
            if (step) {
                instruction = [step valueForKey:@"instruction"];
                distance = [step valueForKey:@"distanceString"];
                street = [step valueForKey:@"roadName"];
            }
        } @catch (NSException *e) { /* key doesn't exist — ok */ }

        @try {
            if (!instruction) {
                id maneuver = [ss valueForKey:@"maneuver"];
                if (maneuver) {
                    instruction = [maneuver valueForKey:@"instructionString"];
                    if (!distance)
                        distance = [maneuver valueForKey:@"distanceRemainingString"];
                    if (!street)
                        street = [maneuver valueForKey:@"name"];
                }
            }
        } @catch (NSException *e) { }

        /* Fallback: scan view hierarchy */
        if (!instruction && ss.isViewLoaded) {
            instruction = findNavLabel(ss.view, 5);
            if (!distance)
                distance = findDistanceLabel(ss.view, 5);
        }

        sendManeuver(instruction, distance, street, @"Apple Maps");
    });
}

- (void)viewWillDisappear:(BOOL)animated {
    stopPollTimer(self);
    %orig;
}

- (void)dealloc {
    stopPollTimer(self);
    %orig;
}

%end

/* Navigation session end */
%hook MNNavigationViewController

- (void)viewDidDisappear:(BOOL)animated {
    %orig;
    [[NavRelayBLE shared] sendNavEnd];
    s_lastInstruction = nil;
    NSLog(@"[NavRelay] Apple Maps navigation ended");
}

%end

/* ==================== Google Maps Hooks ==================== */
/*
 * Timer-based: start when the banner view moves to a window.
 * Polls subview labels every 1.5 seconds.
 */

%hook GMSNavigationBannerView

- (void)didMoveToWindow {
    %orig;

    if (self.window) {
        __weak GMSNavigationBannerView *weakSelf = self;
        startPollTimer(self, ^{
            GMSNavigationBannerView *ss = weakSelf;
            if (!ss || !ss.window) return;

            NSString *instruction = nil;
            NSString *distance = nil;

            @try {
                NSArray *subs = [ss.subviews copy];
                for (UIView *sub in subs) {
                    NSArray *subs2 = [sub.subviews copy];
                    for (UIView *sub2 in subs2) {
                        if (![sub2 isKindOfClass:[UILabel class]]) continue;
                        NSString *text = ((UILabel *)sub2).text;
                        if (!text) continue;
                        if (!distance && isDistanceText(text)) {
                            distance = text;
                        } else if (!instruction && text.length > 3) {
                            instruction = text;
                        }
                    }
                }
            } @catch (NSException *e) { }

            sendManeuver(instruction, distance, nil, @"Google Maps");
        });
    } else {
        stopPollTimer(self);
    }
}

- (void)dealloc {
    stopPollTimer(self);
    %orig;
}

%end

/* ==================== Yandex Navigator Hooks ==================== */

%hook YMKTurnInstructionView

- (void)didMoveToWindow {
    %orig;

    if (self.window) {
        __weak YMKTurnInstructionView *weakSelf = self;
        startPollTimer(self, ^{
            YMKTurnInstructionView *ss = weakSelf;
            if (!ss || !ss.window) return;

            NSString *instruction = nil;
            NSString *distance = nil;

            @try {
                NSArray *subs = [ss.subviews copy];
                for (UIView *sub in subs) {
                    if (![sub isKindOfClass:[UILabel class]]) continue;
                    NSString *text = ((UILabel *)sub).text;
                    if (!text || text.length < 2) continue;
                    if (!distance && isDistanceText(text)) {
                        distance = text;
                    } else if (!instruction && text.length > 3) {
                        instruction = text;
                    }
                }
            } @catch (NSException *e) { }

            sendManeuver(instruction, distance, nil, @"Yandex Navi");
        });
    } else {
        stopPollTimer(self);
    }
}

- (void)dealloc {
    stopPollTimer(self);
    %orig;
}

%end

/* ==================== Tweak Initialization ==================== */

%ctor {
    @autoreleasepool {
        NSString *bundleID = [[NSBundle mainBundle] bundleIdentifier];
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
        if (!supported) return;

        NSLog(@"[NavRelay] v1.1.0 loaded in %@", bundleID);

        /* Delayed BLE init — wait for app to fully load */
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 3 * NSEC_PER_SEC),
                       dispatch_get_main_queue(), ^{
            [[NavRelayBLE shared] ensureBLE];
        });
    }
}
