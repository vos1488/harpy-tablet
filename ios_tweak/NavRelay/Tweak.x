/*
 * NavRelay v1.3.0 — Universal window-scanning approach.
 *
 * Instead of hooking app-specific classes (which change every version),
 * we use a single global NSTimer that scans the entire keyWindow
 * view hierarchy for navigation-like UILabel texts.
 *
 * Works for ANY navigation app without knowing class names.
 *
 * NO %hook on app-specific classes.
 * NO dealloc hooks.
 * NO layout hooks.
 * Just a timer + window scan.
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
            NSLog(@"[NavRelay] Found HARPY via HID: %@", p.name);
            self.connectedPeripheral = p;
            p.delegate = self;
            [self.centralManager connectPeripheral:p options:nil];
            return;
        }
    }

    /* Also try Nav service UUID directly */
    CBUUID *navSvc = [CBUUID UUIDWithString:kNavServiceUUID];
    NSArray *navConnected = [self.centralManager
        retrieveConnectedPeripheralsWithServices:@[navSvc]];
    for (CBPeripheral *p in navConnected) {
        if ([p.name containsString:@"HARPY"]) {
            NSLog(@"[NavRelay] Found HARPY via Nav svc: %@", p.name);
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

- (void)centralManagerDidUpdateState:(CBCentralManager *)central {
    NSLog(@"[NavRelay] BLE state: %ld", (long)central.state);
    if (central.state == CBManagerStatePoweredOn) [self tryConnect];
}

- (void)centralManager:(CBCentralManager *)central
    didDiscoverPeripheral:(CBPeripheral *)peripheral
    advertisementData:(NSDictionary *)ad RSSI:(NSNumber *)RSSI {
    NSString *name = peripheral.name ?: ad[CBAdvertisementDataLocalNameKey];
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
    NSLog(@"[NavRelay] Connected to %@, discovering nav service...", peripheral.name);
    [peripheral discoverServices:@[[CBUUID UUIDWithString:kNavServiceUUID]]];
}

- (void)centralManager:(CBCentralManager *)central
    didDisconnectPeripheral:(CBPeripheral *)peripheral error:(NSError *)error {
    NSLog(@"[NavRelay] Disconnected: %@", error);
    self.isReady = NO;
    self.navCharacteristic = nil;
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 2 * NSEC_PER_SEC),
                   dispatch_get_global_queue(0, 0), ^{ [self tryConnect]; });
}

- (void)centralManager:(CBCentralManager *)central
    didFailToConnectPeripheral:(CBPeripheral *)peripheral error:(NSError *)error {
    NSLog(@"[NavRelay] Failed to connect: %@", error);
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 3 * NSEC_PER_SEC),
                   dispatch_get_global_queue(0, 0), ^{ [self tryConnect]; });
}

- (void)peripheral:(CBPeripheral *)peripheral didDiscoverServices:(NSError *)error {
    if (error) { NSLog(@"[NavRelay] Svc err: %@", error); return; }
    CBUUID *svcUUID = [CBUUID UUIDWithString:kNavServiceUUID];
    for (CBService *svc in peripheral.services) {
        NSLog(@"[NavRelay] Found svc: %@", svc.UUID);
        if ([svc.UUID isEqual:svcUUID])
            [peripheral discoverCharacteristics:
                @[[CBUUID UUIDWithString:kNavDataUUID]] forService:svc];
    }
}

- (void)peripheral:(CBPeripheral *)peripheral
    didDiscoverCharacteristicsForService:(CBService *)service error:(NSError *)error {
    if (error) { NSLog(@"[NavRelay] Chr err: %@", error); return; }
    CBUUID *chrUUID = [CBUUID UUIDWithString:kNavDataUUID];
    for (CBCharacteristic *chr in service.characteristics) {
        if ([chr.UUID isEqual:chrUUID]) {
            self.navCharacteristic = chr;
            self.isReady = YES;
            NSLog(@"[NavRelay] *** READY — Nav characteristic found ***");
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
    /* Check specific phrases first (before generic left/right) */
    if ([l containsString:@"плавно налево"] || [l containsString:@"левее"] ||
        [l containsString:@"slight left"] || [l containsString:@"bear left"] ||
        [l containsString:@"keep left"])
        return NavDirectionSlightLeft;
    if ([l containsString:@"плавно направо"] || [l containsString:@"правее"] ||
        [l containsString:@"slight right"] || [l containsString:@"bear right"] ||
        [l containsString:@"keep right"])
        return NavDirectionSlightRight;
    if ([l containsString:@"налево"] || [l containsString:@"лево"] ||
        [l containsString:@"turn left"] || [l containsString:@"left"])
        return NavDirectionLeft;
    if ([l containsString:@"направо"] || [l containsString:@"право"] ||
        [l containsString:@"turn right"] || [l containsString:@"right"])
        return NavDirectionRight;
    if ([l containsString:@"развернитесь"] || [l containsString:@"разворот"] ||
        [l containsString:@"u-turn"] || [l containsString:@"u turn"] ||
        [l containsString:@"make a u"])
        return NavDirectionUTurn;
    if ([l containsString:@"прибыли"] || [l containsString:@"место назначения"] ||
        [l containsString:@"arrive"] || [l containsString:@"destination"] ||
        [l containsString:@"you have reached"])
        return NavDirectionArrive;
    if ([l containsString:@"круговое"] || [l containsString:@"кольц"] ||
        [l containsString:@"roundabout"] || [l containsString:@"rotary"])
        return NavDirectionRoundabout;
    if ([l containsString:@"прямо"] || [l containsString:@"straight"] ||
        [l containsString:@"continue"] || [l containsString:@"head"])
        return NavDirectionStraight;
    return NavDirectionStraight;
}

static NSString *s_lastInstruction = nil;
static NSString *s_appName = nil;

static void sendManeuver(NSString *instruction, NSString *distance, NSString *street) {
    if (!instruction || instruction.length == 0) return;
    if (s_lastInstruction && [instruction isEqualToString:s_lastInstruction]) return;
    s_lastInstruction = [instruction copy];
    NavDirection dir = parseDirection(instruction);
    NSString *data = [NSString stringWithFormat:@"%ld|%@|%@|%@|||%@",
                      (long)dir, distance ?: @"", instruction ?: @"",
                      street ?: @"", s_appName ?: @"Maps"];
    [[NavRelayBLE shared] sendNavData:data];
    NSLog(@"[NavRelay] >>> Sent: %@", data);
}

/* ==================== Universal Window Scanner ==================== */
/*
 * Scans the entire keyWindow view hierarchy for UILabels.
 * Collects ALL visible labels, then categorizes:
 *   - Navigation instruction (turn left, continue, etc.)
 *   - Distance (ends with m, km, mi, ft, м, км)
 *   - Street name (longer text near nav instruction)
 *
 * This works for ANY app — no class name dependency.
 */

static BOOL isNavText(NSString *t) {
    if (!t || t.length < 3 || t.length > 300) return NO;
    NSString *l = [t lowercaseString];
    return ([l containsString:@"turn"] || [l containsString:@"continue"] ||
            [l containsString:@"left"] || [l containsString:@"right"] ||
            [l containsString:@"arrive"] || [l containsString:@"head"] ||
            [l containsString:@"merge"] || [l containsString:@"exit"] ||
            [l containsString:@"keep"] || [l containsString:@"onto"] ||
            [l containsString:@"take"] || [l containsString:@"ramp"] ||
            [l containsString:@"fork"] || [l containsString:@"stay"] ||
            [l containsString:@"roundabout"] || [l containsString:@"u-turn"] ||
            [l containsString:@"поверн"] || [l containsString:@"прямо"] ||
            [l containsString:@"налево"] || [l containsString:@"направо"] ||
            [l containsString:@"развор"] || [l containsString:@"съезд"] ||
            [l containsString:@"через"] || [l containsString:@"выезд"]);
}

static BOOL isDistanceText(NSString *t) {
    if (!t || t.length < 2 || t.length > 30) return NO;
    NSString *l = [t lowercaseString];
    /* Check suffix patterns */
    if ([l hasSuffix:@" m"] || [l hasSuffix:@" km"] ||
        [l hasSuffix:@" mi"] || [l hasSuffix:@" ft"] ||
        [l hasSuffix:@" м"] || [l hasSuffix:@" км"] ||
        [l hasSuffix:@" yd"] || [l hasSuffix:@" mi"]) return YES;
    /* Also check patterns like "100m", "2.5km", "500 м" */
    NSRange r = [l rangeOfString:@"\\d+\\s*(m|km|mi|ft|м|км|yd)$"
                         options:NSRegularExpressionSearch];
    return r.location != NSNotFound;
}

static BOOL isEtaText(NSString *t) {
    if (!t || t.length < 3 || t.length > 30) return NO;
    NSString *l = [t lowercaseString];
    return ([l containsString:@"min"] || [l containsString:@"мин"] ||
            [l containsString:@"hr"] || [l containsString:@"час"] ||
            [l containsString:@"eta"] ||
            [l rangeOfString:@"\\d{1,2}:\\d{2}" options:NSRegularExpressionSearch].location != NSNotFound);
}

/* Collect all visible labels from a view hierarchy (max depth 8) */
static void collectLabels(UIView *root, NSInteger depth,
                          NSMutableArray *navLabels,
                          NSMutableArray *distLabels,
                          NSMutableArray *etaLabels,
                          NSMutableArray *otherLabels) {
    if (!root || depth <= 0) return;
    @try {
        /* Skip hidden views */
        if (root.isHidden || root.alpha < 0.1) return;

        NSArray *subs = [root.subviews copy];
        for (UIView *sub in subs) {
            if ([sub isKindOfClass:[UILabel class]]) {
                UILabel *lbl = (UILabel *)sub;
                if (lbl.isHidden || lbl.alpha < 0.1) continue;
                NSString *text = lbl.text;
                if (!text || text.length < 2) continue;

                if (isNavText(text)) {
                    [navLabels addObject:text];
                } else if (isDistanceText(text)) {
                    [distLabels addObject:text];
                } else if (isEtaText(text)) {
                    [etaLabels addObject:text];
                } else if (text.length > 2 && text.length < 100) {
                    [otherLabels addObject:text];
                }
            }
            collectLabels(sub, depth - 1, navLabels, distLabels, etaLabels, otherLabels);
        }
    } @catch (NSException *e) { }
}

/* Get the key window (works on iOS 13+) */
static UIWindow *getKeyWindow(void) {
    @try {
        /* iOS 15+ */
        for (UIScene *scene in [UIApplication sharedApplication].connectedScenes) {
            if (![scene isKindOfClass:[UIWindowScene class]]) continue;
            UIWindowScene *ws = (UIWindowScene *)scene;
            if (ws.activationState != UISceneActivationStateForegroundActive) continue;
            for (UIWindow *w in ws.windows) {
                if (w.isKeyWindow) return w;
            }
        }
    } @catch (NSException *e) { }
    /* Fallback */
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    return [UIApplication sharedApplication].keyWindow;
#pragma clang diagnostic pop
}

/* ==================== Global Poll Timer ==================== */

static NSTimer *s_pollTimer = nil;
static NSInteger s_emptyPollCount = 0;

static void pollWindowForNav(void) {
    @try {
        UIWindow *win = getKeyWindow();
        if (!win) return;

        NSMutableArray *navLabels = [NSMutableArray array];
        NSMutableArray *distLabels = [NSMutableArray array];
        NSMutableArray *etaLabels = [NSMutableArray array];
        NSMutableArray *otherLabels = [NSMutableArray array];

        collectLabels(win, 8, navLabels, distLabels, etaLabels, otherLabels);

        if (navLabels.count > 0) {
            s_emptyPollCount = 0;
            NSString *instruction = navLabels.firstObject;
            NSString *distance = distLabels.count > 0 ? distLabels.firstObject : nil;
            NSString *street = nil;

            /* Try to find a street name from other labels
             * (usually near the nav instruction, short-ish text) */
            for (NSString *t in otherLabels) {
                if (t.length > 3 && t.length < 80 &&
                    ![t isEqualToString:instruction] &&
                    !isDistanceText(t) && !isEtaText(t)) {
                    street = t;
                    break;
                }
            }

            sendManeuver(instruction, distance, street);
        } else {
            /* No nav labels found — might have stopped navigating */
            s_emptyPollCount++;
            if (s_emptyPollCount > 10 && s_lastInstruction) {
                /* 10 empty polls (20 sec) → assume navigation ended */
                NSLog(@"[NavRelay] No nav labels for 20s, sending NAV_END");
                [[NavRelayBLE shared] sendNavEnd];
                s_lastInstruction = nil;
            }
        }
    } @catch (NSException *e) {
        NSLog(@"[NavRelay] Poll error: %@", e);
    }
}

/* ==================== Tweak Initialization ==================== */
/*
 * NO %hook, NO %group, NO %init(groups).
 * Just start a timer and scan the window.
 */

%ctor {
    @autoreleasepool {
        NSString *bid = [[NSBundle mainBundle] bundleIdentifier];
        if (!bid) return;

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
            if ([bid isEqualToString:app]) { supported = YES; break; }
        }
        if (!supported) return;

        /* Determine friendly app name */
        if ([bid hasPrefix:@"com.apple.Maps"]) s_appName = @"Apple Maps";
        else if ([bid hasPrefix:@"com.google"]) s_appName = @"Google Maps";
        else if ([bid hasPrefix:@"ru.yandex"]) s_appName = @"Yandex Navi";
        else if ([bid hasPrefix:@"ru.dublgis"]) s_appName = @"2GIS";
        else if ([bid hasPrefix:@"com.waze"]) s_appName = @"Waze";
        else s_appName = @"Maps";

        NSLog(@"[NavRelay] v1.3.0 loaded in %@ (%@)", bid, s_appName);

        /* Start poll timer after app is fully loaded (5 sec delay) */
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC),
                       dispatch_get_main_queue(), ^{
            /* Init BLE */
            [[NavRelayBLE shared] ensureBLE];

            /* Start the universal window scanner (every 2 seconds) */
            s_pollTimer = [NSTimer scheduledTimerWithTimeInterval:2.0
                                                         repeats:YES
                                                           block:^(NSTimer *t) {
                pollWindowForNav();
            }];
            NSLog(@"[NavRelay] Universal window scanner started (2s interval)");
        });
    }
}
