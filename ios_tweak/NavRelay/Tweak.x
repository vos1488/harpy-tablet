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

/* Category for methods added via %new */
@interface UIViewController (NavRelay)
- (NSString *)dumpMethods;
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
        _centralManager = [[CBCentralManager alloc]
            initWithDelegate:self
            queue:dispatch_get_global_queue(DISPATCH_QUEUE_PRIORITY_DEFAULT, 0)
            options:@{CBCentralManagerOptionRestoreIdentifierKey: @"NavRelayBLE"}];
        _isReady = NO;
        NSLog(@"[NavRelay] BLE Manager initialized");
    }
    return self;
}

#pragma mark - Public API

- (void)sendNavData:(NSString *)data {
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
    if (self.connectedPeripheral && self.connectedPeripheral.state == CBPeripheralStateConnected) {
        return; /* Already connected */
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
    willRestoreState:(NSDictionary<NSString *,id> *)dict {
    NSArray *peripherals = dict[CBCentralManagerRestoredStatePeripheralsKey];
    for (CBPeripheral *p in peripherals) {
        if ([p.name containsString:@"HARPY"]) {
            self.connectedPeripheral = p;
            p.delegate = self;
            NSLog(@"[NavRelay] Restored peripheral: %@", p.name);
        }
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

/*
 * Hook: MNStepCardViewController (iOS 15-17)
 * Called when the turn card updates its maneuver view.
 */
%hook MNStepCardViewController

- (void)viewDidLayoutSubviews {
    %orig;

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

        /* Fallback: scan view hierarchy for labels */
        if (!instruction) {
            instruction = [self findLabelText:self.view tag:100];
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

/* ==================== Universal Fallback Hook ==================== */
/*
 * Hooks UIViewController's viewDidLayoutSubviews for VCs whose
 * class name contains "Navigation", "Maneuver", or "Step".
 * This catches unknown navigation apps.
 */

%hook UIViewController

- (void)viewDidLayoutSubviews {
    %orig;

    @try {
        NSString *className = NSStringFromClass([self class]);

        /* Only process navigation-related VCs */
        if (!([className containsString:@"Navigation"] ||
              [className containsString:@"Maneuver"] ||
              [className containsString:@"Step"] ||
              [className containsString:@"TurnCard"] ||
              [className containsString:@"Direction"])) {
            return;
        }

        /* Log class name for debugging (helps find correct hooks) */
        static NSMutableSet *loggedClasses = nil;
        if (!loggedClasses) loggedClasses = [NSMutableSet set];
        if (![loggedClasses containsObject:className]) {
            [loggedClasses addObject:className];
            NSLog(@"[NavRelay] Detected nav VC: %@ (methods: %@)",
                  className, [self dumpMethods]);
        }
    } @catch (NSException *ex) {
        /* Ignore */
    }
}

%new
- (NSString *)dumpMethods {
    unsigned int count = 0;
    Method *methods = class_copyMethodList([self class], &count);
    NSMutableArray *names = [NSMutableArray array];
    int limit = MIN(count, 20);
    for (int i = 0; i < limit; i++) {
        [names addObject:NSStringFromSelector(method_getName(methods[i]))];
    }
    free(methods);
    return [names componentsJoinedByString:@", "];
}

%end

/* ==================== Tweak Initialization ==================== */

%ctor {
    NSLog(@"[NavRelay] Tweak loaded! Initializing BLE relay...");
    [NavRelayBLE shared];

    /* Log all classes containing Nav/Step/Maneuver for debugging */
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC),
                   dispatch_get_main_queue(), ^{
        int numClasses = objc_getClassList(NULL, 0);
        Class *classes = (Class *)malloc(sizeof(Class) * numClasses);
        objc_getClassList(classes, numClasses);

        NSLog(@"[NavRelay] === Navigation-related classes ===");
        for (int i = 0; i < numClasses; i++) {
            NSString *name = NSStringFromClass(classes[i]);
            if ([name containsString:@"Navigation"] ||
                [name containsString:@"Maneuver"] ||
                [name containsString:@"StepCard"] ||
                [name containsString:@"TurnBanner"] ||
                [name containsString:@"MNStep"]) {
                NSLog(@"[NavRelay]   → %@", name);
            }
        }
        free(classes);
        NSLog(@"[NavRelay] === End class dump ===");
    });
}
