/*
 * NavRelay v1.2.0 — Jailbreak Tweak for relaying turn-by-turn navigation
 *                    from iPhone to HARPY Remote (ESP32) via custom BLE GATT.
 *
 * ARCHITECTURE:
 *   - %group per app (Apple Maps / Google Maps / Yandex)
 *   - %init only for classes that EXIST at runtime (no crash on missing class)
 *   - NO dealloc hooks (dangerous with Substrate)
 *   - NO layout hooks (viewDidLayoutSubviews / layoutSubviews)
 *   - Single global NSTimer polls the active navigation VC
 *   - Weak references everywhere to avoid retain cycles
 *
 * Service UUID:  E6A30000-B5A3-F393-E0A9-E50E24DCCA9E
 * Nav Data UUID: E6A30001-B5A3-F393-E0A9-E50E24DCCA9E
 * Data format:   "DIR|DIST|INSTRUCTION|STREET|ETA|SPEED|APP"
 */

#import <Foundation/Foundation.h>
#import <CoreBluetooth/CoreBluetooth.h>
#import <UIKit/UIKit.h>
#import <objc/runtime.h>

/* ==================== Constants ==================== */

static NSString *const kNavServiceUUID = @"E6A30000-B5A3-F393-E0A9-E50E24DCCA9E";
static NSString *const kNavDataUUID    = @"E6A30001-B5A3-F393-E0A9-E50E24DCCA9E";

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
    if (self) { _isReady = NO; }
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

- (void)sendNavData:(NSString *)data {
    if (!data) return;
    [self ensureBLE];
    if (self.isReady && self.navCharacteristic && self.connectedPeripheral) {
        NSData *bytes = [data dataUsingEncoding:NSUTF8StringEncoding];
        if (!bytes) return;
        if (bytes.length > 512)
            bytes = [bytes subdataWithRange:NSMakeRange(0, 512)];
        @try {
            [self.connectedPeripheral writeValue:bytes
                               forCharacteristic:self.navCharacteristic
                                            type:CBCharacteristicWriteWithResponse];
        } @catch (NSException *ex) {
            NSLog(@"[NavRelay] Write ex: %@", ex);
            self.isReady = NO;
        }
    } else {
        self.pendingData = data;
        [self tryConnect];
    }
}

- (void)sendNavEnd { [self sendNavData:@"NAV_END"]; }

- (void)tryConnect {
    [self ensureBLE];
    if (self.connectedPeripheral &&
        self.connectedPeripheral.state == CBPeripheralStateConnected) return;
    if (self.centralManager.state != CBManagerStatePoweredOn) return;

    CBUUID *hidSvc = [CBUUID UUIDWithString:@"1812"];
    NSArray *connected = [self.centralManager
        retrieveConnectedPeripheralsWithServices:@[hidSvc]];
    for (CBPeripheral *p in connected) {
        if ([p.name containsString:@"HARPY"]) {
            self.connectedPeripheral = p;
            p.delegate = self;
            [self.centralManager connectPeripheral:p options:nil];
            return;
        }
    }
    [self.centralManager scanForPeripheralsWithServices:nil options:nil];
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 10 * NSEC_PER_SEC),
                   dispatch_get_global_queue(0, 0), ^{
        [self.centralManager stopScan];
    });
}

- (void)centralManagerDidUpdateState:(CBCentralManager *)central {
    if (central.state == CBManagerStatePoweredOn) [self tryConnect];
}

- (void)centralManager:(CBCentralManager *)central
    didDiscoverPeripheral:(CBPeripheral *)peripheral
    advertisementData:(NSDictionary *)ad RSSI:(NSNumber *)RSSI {
    NSString *name = peripheral.name ?: ad[CBAdvertisementDataLocalNameKey];
    if ([name containsString:@"HARPY"]) {
        [central stopScan];
        self.connectedPeripheral = peripheral;
        peripheral.delegate = self;
        [central connectPeripheral:peripheral options:nil];
    }
}

- (void)centralManager:(CBCentralManager *)central
    didConnectPeripheral:(CBPeripheral *)peripheral {
    [peripheral discoverServices:@[[CBUUID UUIDWithString:kNavServiceUUID]]];
}

- (void)centralManager:(CBCentralManager *)central
    didDisconnectPeripheral:(CBPeripheral *)peripheral error:(NSError *)error {
    self.isReady = NO;
    self.navCharacteristic = nil;
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 2 * NSEC_PER_SEC),
                   dispatch_get_global_queue(0, 0), ^{ [self tryConnect]; });
}

- (void)peripheral:(CBPeripheral *)peripheral didDiscoverServices:(NSError *)error {
    if (error) return;
    CBUUID *svcUUID = [CBUUID UUIDWithString:kNavServiceUUID];
    for (CBService *svc in peripheral.services) {
        if ([svc.UUID isEqual:svcUUID])
            [peripheral discoverCharacteristics:
                @[[CBUUID UUIDWithString:kNavDataUUID]] forService:svc];
    }
}

- (void)peripheral:(CBPeripheral *)peripheral
    didDiscoverCharacteristicsForService:(CBService *)service error:(NSError *)error {
    if (error) return;
    CBUUID *chrUUID = [CBUUID UUIDWithString:kNavDataUUID];
    for (CBCharacteristic *chr in service.characteristics) {
        if ([chr.UUID isEqual:chrUUID]) {
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
    didWriteValueForCharacteristic:(CBCharacteristic *)c error:(NSError *)error {
    if (error) NSLog(@"[NavRelay] Write err: %@", error);
}

@end

/* ==================== Direction Parser ==================== */

static NavDirection parseDirection(NSString *text) {
    if (!text) return NavDirectionStraight;
    NSString *l = [text lowercaseString];
    if ([l containsString:@"плавно налево"] || [l containsString:@"левее"] ||
        [l containsString:@"slight left"] || [l containsString:@"bear left"])
        return NavDirectionSlightLeft;
    if ([l containsString:@"плавно направо"] || [l containsString:@"правее"] ||
        [l containsString:@"slight right"] || [l containsString:@"bear right"])
        return NavDirectionSlightRight;
    if ([l containsString:@"налево"] || [l containsString:@"лево"] ||
        [l containsString:@"turn left"] || [l containsString:@" left"])
        return NavDirectionLeft;
    if ([l containsString:@"направо"] || [l containsString:@"право"] ||
        [l containsString:@"turn right"] || [l containsString:@" right"])
        return NavDirectionRight;
    if ([l containsString:@"развернитесь"] || [l containsString:@"разворот"] ||
        [l containsString:@"u-turn"] || [l containsString:@"u turn"])
        return NavDirectionUTurn;
    if ([l containsString:@"прибыли"] || [l containsString:@"место назначения"] ||
        [l containsString:@"arrive"] || [l containsString:@"destination"])
        return NavDirectionArrive;
    if ([l containsString:@"круговое"] || [l containsString:@"кольц"] ||
        [l containsString:@"roundabout"])
        return NavDirectionRoundabout;
    if ([l containsString:@"прямо"] || [l containsString:@"straight"] ||
        [l containsString:@"continue"])
        return NavDirectionStraight;
    return NavDirectionStraight;
}

/* De-duplicate and send */
static NSString *s_lastInstruction = nil;

static void sendManeuver(NSString *instruction, NSString *distance,
                         NSString *street, NSString *appName) {
    if (!instruction || instruction.length == 0) return;
    if (s_lastInstruction && [instruction isEqualToString:s_lastInstruction]) return;
    s_lastInstruction = [instruction copy];
    NavDirection dir = parseDirection(instruction);
    NSString *data = [NSString stringWithFormat:@"%ld|%@|%@|%@|||%@",
                      (long)dir, distance ?: @"", instruction ?: @"",
                      street ?: @"", appName ?: @"Maps"];
    [[NavRelayBLE shared] sendNavData:data];
}

/* ==================== Safe View Scanners ==================== */

static BOOL isNavText(NSString *t) {
    if (!t || t.length < 3 || t.length > 300) return NO;
    NSString *l = [t lowercaseString];
    return ([l containsString:@"turn"] || [l containsString:@"continue"] ||
            [l containsString:@"left"] || [l containsString:@"right"] ||
            [l containsString:@"arrive"] || [l containsString:@"head"] ||
            [l containsString:@"merge"] || [l containsString:@"exit"] ||
            [l containsString:@"поверн"] || [l containsString:@"прямо"] ||
            [l containsString:@"налево"] || [l containsString:@"направо"] ||
            [l containsString:@"развор"] || [l containsString:@"съезд"]);
}

static BOOL isDistanceText(NSString *t) {
    if (!t || t.length < 2 || t.length > 30) return NO;
    NSString *l = [t lowercaseString];
    return ([l hasSuffix:@" m"] || [l hasSuffix:@" km"] ||
            [l hasSuffix:@" mi"] || [l hasSuffix:@" ft"] ||
            [l hasSuffix:@" м"] || [l hasSuffix:@" км"]);
}

static NSString *findLabel(UIView *root, NSInteger depth, BOOL wantDistance) {
    if (!root || depth <= 0) return nil;
    @try {
        NSArray *subs = [root.subviews copy];
        for (UIView *sub in subs) {
            if ([sub isKindOfClass:[UILabel class]]) {
                NSString *text = ((UILabel *)sub).text;
                if (wantDistance ? isDistanceText(text) : isNavText(text))
                    return text;
            }
            NSString *found = findLabel(sub, depth - 1, wantDistance);
            if (found) return found;
        }
    } @catch (NSException *e) { }
    return nil;
}

/* ==================== Global Polling Timer ==================== */
/*
 * Single NSTimer on main run loop. Polls the currently tracked VC/View.
 * Weak references — if the VC dies, we just nil-out and wait for the next one.
 * NO associated objects, NO dealloc hooks.
 */

static NSTimer *s_pollTimer = nil;
static __weak UIViewController *s_activeNavVC = nil;
static __weak UIView *s_activeNavView = nil;
static NSString *s_activeApp = nil;

static void pollNavData(void) {
    @try {
        NSString *instruction = nil;
        NSString *distance = nil;
        NSString *street = nil;

        /* Apple Maps: try KVC on the VC first */
        UIViewController *vc = s_activeNavVC;
        if (vc) {
            @try {
                id step = [vc valueForKey:@"currentStep"];
                if (step) {
                    instruction = [step valueForKey:@"instruction"];
                    distance = [step valueForKey:@"distanceString"];
                    street = [step valueForKey:@"roadName"];
                }
            } @catch (NSException *e) { }

            if (!instruction) {
                @try {
                    id maneuver = [vc valueForKey:@"maneuver"];
                    if (maneuver) {
                        instruction = [maneuver valueForKey:@"instructionString"];
                        if (!distance)
                            distance = [maneuver valueForKey:@"distanceRemainingString"];
                        if (!street)
                            street = [maneuver valueForKey:@"name"];
                    }
                } @catch (NSException *e) { }
            }

            /* Fallback: scan VC's view */
            if (!instruction && vc.isViewLoaded) {
                instruction = findLabel(vc.view, 5, NO);
                if (!distance) distance = findLabel(vc.view, 5, YES);
            }
        }

        /* Google Maps / Yandex: scan the tracked UIView */
        UIView *v = s_activeNavView;
        if (!instruction && v && v.window) {
            instruction = findLabel(v, 4, NO);
            if (!distance) distance = findLabel(v, 4, YES);
        }

        if (instruction) {
            sendManeuver(instruction, distance, street, s_activeApp);
        }
    } @catch (NSException *e) {
        NSLog(@"[NavRelay] Poll error: %@", e);
    }
}

static void ensurePollTimer(void) {
    if (s_pollTimer && [s_pollTimer isValid]) return;
    s_pollTimer = [NSTimer scheduledTimerWithTimeInterval:2.0
                                                 repeats:YES
                                                   block:^(NSTimer *t) {
        pollNavData();
    }];
    NSLog(@"[NavRelay] Global poll timer started");
}

static void stopPollTimerIfIdle(void) {
    if (!s_activeNavVC && !s_activeNavView) {
        [s_pollTimer invalidate];
        s_pollTimer = nil;
        NSLog(@"[NavRelay] Global poll timer stopped (idle)");
    }
}

/* ==================== %group AppleMaps ==================== */
/*
 * Only %init'd if MNStepCardViewController exists at runtime.
 * Hooks viewDidAppear / viewWillDisappear — safe lifecycle methods.
 * NO viewDidLayoutSubviews, NO dealloc.
 */

%group AppleMaps

%hook MNStepCardViewController

- (void)viewDidAppear:(BOOL)animated {
    %orig;
    s_activeNavVC = self;
    s_activeApp = @"Apple Maps";
    ensurePollTimer();
    NSLog(@"[NavRelay] Apple Maps step card appeared");
}

- (void)viewWillDisappear:(BOOL)animated {
    if (s_activeNavVC == self) {
        s_activeNavVC = nil;
        stopPollTimerIfIdle();
    }
    %orig;
}

%end

%hook MNNavigationViewController

- (void)viewDidDisappear:(BOOL)animated {
    %orig;
    s_activeNavVC = nil;
    s_activeNavView = nil;
    stopPollTimerIfIdle();
    [[NavRelayBLE shared] sendNavEnd];
    s_lastInstruction = nil;
    NSLog(@"[NavRelay] Apple Maps navigation ended");
}

%end

%end /* group AppleMaps */

/* ==================== %group GoogleMaps ==================== */

%group GoogleMaps

%hook GMSNavigationBannerView

- (void)didMoveToWindow {
    %orig;
    if (self.window) {
        s_activeNavView = self;
        s_activeApp = @"Google Maps";
        ensurePollTimer();
        NSLog(@"[NavRelay] Google Maps banner appeared");
    } else {
        if (s_activeNavView == self) {
            s_activeNavView = nil;
            stopPollTimerIfIdle();
        }
    }
}

%end

%end /* group GoogleMaps */

/* ==================== %group YandexNavi ==================== */

%group YandexNavi

%hook YMKTurnInstructionView

- (void)didMoveToWindow {
    %orig;
    if (self.window) {
        s_activeNavView = self;
        s_activeApp = @"Yandex Navi";
        ensurePollTimer();
        NSLog(@"[NavRelay] Yandex turn view appeared");
    } else {
        if (s_activeNavView == self) {
            s_activeNavView = nil;
            stopPollTimerIfIdle();
        }
    }
}

%end

%end /* group YandexNavi */

/* ==================== Tweak Initialization ==================== */

%ctor {
    @autoreleasepool {
        NSString *bid = [[NSBundle mainBundle] bundleIdentifier];
        if (!bid) return;

        BOOL isAppleMaps = [bid isEqualToString:@"com.apple.Maps"];
        BOOL isGoogleMaps = [bid isEqualToString:@"com.google.Maps"];
        BOOL isYandex = ([bid isEqualToString:@"ru.yandex.yandexnavi"] ||
                         [bid isEqualToString:@"ru.yandex.mobile.navigator"]);
        BOOL is2GIS = [bid isEqualToString:@"ru.dublgis.dgismobile"];
        BOOL isWaze = [bid isEqualToString:@"com.waze.iphone"];

        if (!isAppleMaps && !isGoogleMaps && !isYandex && !is2GIS && !isWaze)
            return;

        NSLog(@"[NavRelay] v1.2.0 loaded in %@", bid);

        /*
         * CRITICAL: Only %init groups whose classes exist at runtime.
         * Hooking a non-existent class = instant crash.
         */
        if (isAppleMaps) {
            Class cls1 = NSClassFromString(@"MNStepCardViewController");
            Class cls2 = NSClassFromString(@"MNNavigationViewController");
            if (cls1 || cls2) {
                %init(AppleMaps);
                NSLog(@"[NavRelay] Apple Maps hooks active (MNStepCardVC=%p MNNavVC=%p)",
                      cls1, cls2);
            } else {
                NSLog(@"[NavRelay] Apple Maps classes NOT found — hooks skipped");
            }
        }

        if (isGoogleMaps) {
            Class cls = NSClassFromString(@"GMSNavigationBannerView");
            if (cls) {
                %init(GoogleMaps);
                NSLog(@"[NavRelay] Google Maps hooks active");
            } else {
                NSLog(@"[NavRelay] GMSNavigationBannerView NOT found — hooks skipped");
            }
        }

        if (isYandex || is2GIS) {
            Class cls = NSClassFromString(@"YMKTurnInstructionView");
            if (cls) {
                %init(YandexNavi);
                NSLog(@"[NavRelay] Yandex hooks active");
            } else {
                NSLog(@"[NavRelay] YMKTurnInstructionView NOT found — hooks skipped");
            }
        }

        /* Delayed BLE init */
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 3 * NSEC_PER_SEC),
                       dispatch_get_main_queue(), ^{
            [[NavRelayBLE shared] ensureBLE];
        });
    }
}
