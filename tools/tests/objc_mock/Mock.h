#ifndef MOCK_H
#define MOCK_H
#include <stddef.h>
#include <stdint.h>
typedef signed char BOOL;
#define YES ((BOOL)1)
#define NO ((BOOL)0)
#define nil ((id)0)
@class NSString, NSArray, NSDictionary;
typedef double CGFloat;
typedef unsigned long NSUInteger;
typedef struct { unsigned long state; __unsafe_unretained id *itemsPtr; unsigned long *mutationsPtr; unsigned long extra[5]; } NSFastEnumerationState;
@protocol NSFastEnumeration - (NSUInteger)countByEnumeratingWithState:(NSFastEnumerationState *)state objects:(__unsafe_unretained id *)stackbuf count:(NSUInteger)len; @end
typedef unsigned long NSUInteger;
typedef long NSInteger;
#define CGFLOAT_MAX 1.7e308
#define NS_VALID_UNTIL_END_OF_SCOPE __attribute__((objc_precise_lifetime))
#define NSOrderedSame 0
#define M_PI_2 1.57079632679489661923
typedef struct { CGFloat x, y; } CGPoint;
typedef struct { CGFloat width, height; } CGSize;
typedef struct { CGPoint origin; CGSize size; } CGRect;
typedef struct { CGFloat top, left, bottom, right; } UIEdgeInsets;
static const CGRect CGRectZero = {{0,0},{0,0}};
static const CGSize CGSizeZero = {0,0};
static const CGPoint CGPointZero = {0,0};
static inline CGPoint CGPointMake(CGFloat x, CGFloat y){CGPoint p={x,y};return p;}
static inline CGSize CGSizeMake(CGFloat w, CGFloat h){CGSize p={w,h};return p;}
static inline CGRect CGRectMake(CGFloat x, CGFloat y, CGFloat w, CGFloat h){CGRect r={{x,y},{w,h}};return r;}
static inline CGRect CGRectInset(CGRect r, CGFloat dx, CGFloat dy){return r;}
static inline BOOL CGRectContainsPoint(CGRect r, CGPoint p){return 0;}
typedef struct CGColor *CGColorRef;
typedef struct CGPath *CGPathRef;
typedef struct CGImage *CGImageRef;
typedef struct { CGFloat m11,m12,m13,m14,m21,m22,m23,m24,m31,m32,m33,m34,m41,m42,m43,m44; } CATransform3D;
CATransform3D CATransform3DMakeScale(CGFloat,CGFloat,CGFloat);
extern const CATransform3D CATransform3DIdentity;
extern NSString *const kCALineCapRound; extern NSString *const kCALineJoinRound;
typedef id CFTypeRef;
typedef NSUInteger UIViewAutoresizing;
#define UIViewAutoresizingFlexibleWidth 2
#define UIViewAutoresizingFlexibleHeight 16
#define NSRunLoopCommonModes ((NSString*)0)
#define NSDefaultRunLoopMode ((NSString*)0)
typedef NSUInteger UIInterfaceOrientationMask;
#define UIInterfaceOrientationMaskLandscape 24
#define UIWindowLevelNormal 0.0
#define UISceneActivationStateForegroundActive 0
#define UISceneActivationStateForegroundInactive 1
#define NSTextAlignmentRight 2
#define NSTextAlignmentCenter 1
#define UIGestureRecognizerStateBegan 1
#define UIGestureRecognizerStateChanged 2
#define UIGestureRecognizerStateEnded 3
#define UIGestureRecognizerStateCancelled 4
#define UIGestureRecognizerStateFailed 5
#define UIUserInterfaceIdiomPad 1
#define NSDocumentDirectory 9
#define NSUserDomainMask 1
#define UIControlEventValueChanged 4096
#define UIControlEventTouchUpInside 64
#define UIControlEventTouchUpOutside 128
#define UIControlEventTouchCancel 256
#define UIControlStateNormal 0
#define UIControlEvents NSUInteger
typedef NSUInteger UIControlEvents_t;
typedef int UIControlState;
#define UIButtonTypeSystem 1
#define UIFontWeightMedium 0.23
#define UIImpactFeedbackStyleLight 0
#define UIImpactFeedbackStyleMedium 1
#define UIImpactFeedbackStyleHeavy 2
typedef NSInteger UIImpactFeedbackStyle;
#define UIScrollViewContentInsetAdjustmentNever 2
#define UIUserInterfaceStyleDark 2
#define UIInterfaceOrientationLandscapeLeft 4
#define UIInterfaceOrientationLandscapeRight 3
typedef NSInteger UIInterfaceOrientation;
#define NSProcessInfoThermalStateFair 1
#define NSProcessInfoThermalStateSerious 2
#define NSProcessInfoThermalStateCritical 3
#define NSThread_dummy
#define UIImageSymbolWeightMedium 5
#define UIImageSymbolWeightSemibold 6
#define UIImageRenderingModeAlwaysOriginal 1
#define UIViewContentModeRedraw 3
typedef void (^dispatch_block_t)(void);
typedef struct dispatch_queue_s *dispatch_queue_t;
dispatch_queue_t dispatch_get_main_queue(void);
void dispatch_async(dispatch_queue_t, dispatch_block_t);

@protocol NSObject
- (struct _NSZone *)zone __attribute__((unavailable));
- (BOOL)isKindOfClass:(Class)c;
@end
@interface NSObject <NSObject>
+ (instancetype)alloc; - (instancetype)init; + (instancetype)new; + (Class)class;
@end
@interface NSString : NSObject
+ (instancetype)stringWithUTF8String:(const char *)s;
+ (instancetype)stringWithFormat:(NSString *)f, ...;
- (const char *)UTF8String;
- (NSArray *)componentsSeparatedByString:(NSString *)s;
- (NSUInteger)length;
- (double)doubleValue;
- (NSInteger)caseInsensitiveCompare:(NSString *)o;
- (NSString *)uppercaseString;
- (NSString *)stringByAppendingString:(NSString *)s;
@end
@interface NSArray : NSObject <NSFastEnumeration>
- (NSUInteger)count; - (id)objectAtIndex:(NSUInteger)i;
+ (instancetype)arrayWithObjects:(id)o, ...;
- (id)copy;
@end
@interface NSMutableArray : NSArray
+ (instancetype)arrayWithCapacity:(NSUInteger)n;
- (void)addObject:(id)o; - (void)removeAllObjects;
@end
NSArray *NSSearchPathForDirectoriesInDomains(NSUInteger dir, NSUInteger domain, BOOL expand);
@interface NSDictionary : NSObject
+ (instancetype)dictionaryWithObjects:(const __unsafe_unretained id *)o forKeys:(const __unsafe_unretained id *)k count:(NSUInteger)c;
@end
@interface NSNumber : NSObject
+ (NSNumber*)numberWithInt:(int)i;
+ (NSNumber*)numberWithDouble:(double)d;
@end
#define NSQualityOfServiceUserInteractive 0x21
@interface NSThread : NSObject
+ (BOOL)isMainThread;
+ (void)sleepForTimeInterval:(double)s;
- (instancetype)initWithTarget:(id)t selector:(SEL)s object:(id)o;
- (void)start;
@property (copy) NSString *name; @property NSInteger qualityOfService;
@end
@interface NSDate : NSObject
+ (instancetype)dateWithTimeIntervalSinceNow:(double)s;
@end
@interface NSRunLoop : NSObject
+ (NSRunLoop*)mainRunLoop; + (NSRunLoop*)currentRunLoop; - (void)addTimer:(id)t forMode:(NSString*)m;
- (BOOL)runMode:(NSString*)mode beforeDate:(NSDate*)d;
@end
@interface NSSet : NSObject <NSFastEnumeration> - (NSUInteger)count; @end
@interface NSTimer : NSObject
+ (NSTimer*)timerWithTimeInterval:(double)i target:(id)t selector:(SEL)s userInfo:(id)u repeats:(BOOL)r;
+ (NSTimer*)scheduledTimerWithTimeInterval:(double)i target:(id)t selector:(SEL)s userInfo:(id)u repeats:(BOOL)r;
@property double tolerance; - (void)invalidate;
@end
@interface NSOperationQueue : NSObject
+ (NSOperationQueue*)mainQueue;
@end
@interface NSError : NSObject @end
@interface NSProcessInfo : NSObject
+ (NSProcessInfo*)processInfo; @property (readonly) NSInteger thermalState;
@property (readonly, getter=isLowPowerModeEnabled) BOOL lowPowerModeEnabled;
@end
@interface NSShadow : NSObject @end
extern NSString *const NSFontAttributeName;
extern NSString *const NSForegroundColorAttributeName;
extern NSString *const NSKernAttributeName;

@interface UIColor : NSObject
+ (UIColor*)colorWithRed:(CGFloat)r green:(CGFloat)g blue:(CGFloat)b alpha:(CGFloat)a;
+ (UIColor*)colorWithWhite:(CGFloat)w alpha:(CGFloat)a;
+ (UIColor*)clearColor; + (UIColor*)blackColor; + (UIColor*)whiteColor; + (UIColor*)lightGrayColor;
- (UIColor*)colorWithAlphaComponent:(CGFloat)a;
@property (readonly) CGColorRef CGColor;
@end
@interface UIFont : NSObject
+ (UIFont*)boldSystemFontOfSize:(CGFloat)s; + (UIFont*)systemFontOfSize:(CGFloat)s;
+ (UIFont*)monospacedDigitSystemFontOfSize:(CGFloat)s weight:(CGFloat)w;
@end
@interface UIImageSymbolConfiguration : NSObject
+ (instancetype)configurationWithPointSize:(CGFloat)p weight:(NSInteger)w;
@end
@interface UIImage : NSObject
+ (UIImage*)systemImageNamed:(NSString*)n withConfiguration:(UIImageSymbolConfiguration*)c;
- (UIImage*)imageWithTintColor:(UIColor*)c renderingMode:(NSInteger)m;
@property (readonly) CGSize size; @property (readonly) CGFloat scale; @property (readonly) CGImageRef CGImage;
- (void)drawInRect:(CGRect)r;
@end
@interface UIBezierPath : NSObject
+ (UIBezierPath*)bezierPath;
+ (UIBezierPath*)bezierPathWithOvalInRect:(CGRect)r;
+ (UIBezierPath*)bezierPathWithRoundedRect:(CGRect)r cornerRadius:(CGFloat)c;
- (void)moveToPoint:(CGPoint)p; - (void)addLineToPoint:(CGPoint)p;
- (void)addQuadCurveToPoint:(CGPoint)p controlPoint:(CGPoint)c; - (void)closePath;
@property (readonly) CGPathRef CGPath; @property (readonly, getter=isEmpty) BOOL empty;
@end
@interface UIGraphicsImageRendererContext : NSObject @end
@interface UIGraphicsImageRendererFormat : NSObject
+ (instancetype)preferredFormat; @property BOOL opaque; @property CGFloat scale;
@end
@interface UIGraphicsImageRenderer : NSObject
- (instancetype)initWithSize:(CGSize)s format:(UIGraphicsImageRendererFormat*)f;
- (UIImage*)imageWithActions:(void (^)(UIGraphicsImageRendererContext*))a;
@end
@interface NSMutableString : NSString
+ (instancetype)stringWithCapacity:(NSUInteger)n;
- (void)appendString:(NSString *)s; - (void)appendFormat:(NSString *)f, ...;
@end

@interface NSString (UIKitAdditions)
- (CGSize)sizeWithAttributes:(NSDictionary*)a;
- (void)drawAtPoint:(CGPoint)p withAttributes:(NSDictionary*)a;
@end

@interface CAAnimation : NSObject @end
@interface CALayer : NSObject
+ (instancetype)layer;
@property CGRect frame, bounds; @property CGPoint position; @property CGFloat contentsScale;
@property CATransform3D transform; @property float opacity; @property BOOL hidden;
@property CGFloat cornerRadius; @property BOOL masksToBounds; @property (strong) CALayer *mask;
@property CGColorRef backgroundColor; @property id contents; @property (readonly, copy) NSArray *sublayers;
@property CGFloat borderWidth; @property CGColorRef borderColor;
@property CGColorRef shadowColor; @property float shadowOpacity; @property CGFloat shadowRadius;
@property CGSize shadowOffset; @property CGPathRef shadowPath;
- (void)addSublayer:(CALayer*)l; - (void)removeFromSuperlayer;
@end
@interface CAMetalLayer : CALayer @property CGSize drawableSize; @end
@interface CAShapeLayer : CALayer
@property CGPathRef path; @property CGColorRef fillColor, strokeColor; @property CGFloat lineWidth;
@property (copy) NSString *lineCap, *lineJoin; @property (copy) NSArray *lineDashPattern;
@end
@interface CAGradientLayer : CALayer
@property (copy) NSArray *colors; @property CGPoint startPoint, endPoint;
@end
@interface CATransaction : NSObject
+ (void)begin; + (void)commit; + (void)setDisableActions:(BOOL)f; + (void)setAnimationDuration:(double)d;
@end
typedef struct { float minimum, maximum, preferred; } CAFrameRateRange;
static inline CAFrameRateRange CAFrameRateRangeMake(float a, float b, float c)
{ CAFrameRateRange r = { a, b, c }; return r; }
double CACurrentMediaTime(void);
@interface CADisplayLink : NSObject
+ (CADisplayLink*)displayLinkWithTarget:(id)t selector:(SEL)s;
@property (readonly) double timestamp, targetTimestamp;
@property CAFrameRateRange preferredFrameRateRange;
@property NSInteger preferredFramesPerSecond; @property (getter=isPaused) BOOL paused;
- (void)addToRunLoop:(NSRunLoop*)r forMode:(NSString*)m; - (void)invalidate;
@end

@interface UIEvent : NSObject @end
@class UIView;
@interface UIGestureRecognizer : NSObject
- (instancetype)initWithTarget:(id)t action:(SEL)a;
@property (readonly) NSInteger state; @property (readonly) UIView *view;
- (CGPoint)locationInView:(UIView*)v;
@end
@interface UITapGestureRecognizer : UIGestureRecognizer @end
@interface UIPanGestureRecognizer : UIGestureRecognizer
@property NSUInteger maximumNumberOfTouches, minimumNumberOfTouches;
- (CGPoint)translationInView:(UIView*)v;
@end
@interface UIPinchGestureRecognizer : UIGestureRecognizer @property CGFloat scale; @end
@interface UIDevice : NSObject
+ (UIDevice*)currentDevice; @property (readonly) NSInteger userInterfaceIdiom;
@property (getter=isBatteryMonitoringEnabled) BOOL batteryMonitoringEnabled; @property (readonly) float batteryLevel;
@end
@class UIWindow, UIScreen;
@interface UIView : NSObject
- (id)initWithFrame:(CGRect)f;
@property CGRect frame, bounds; @property (readonly) CALayer *layer; @property CGFloat alpha;
@property BOOL hidden, userInteractionEnabled, opaque, clipsToBounds; @property NSInteger tag;
@property (strong) UIColor *backgroundColor; @property (readonly) NSArray *subviews;
@property UIViewAutoresizing autoresizingMask; @property NSInteger overrideUserInterfaceStyle;
@property (readonly) UIEdgeInsets safeAreaInsets; @property CGFloat cornerRadius_unused;
@property (nonatomic, copy) NSString *accessibilityLabel;
- (UIView*)hitTest:(CGPoint)p withEvent:(UIEvent*)e; - (CGPoint)convertPoint:(CGPoint)p toView:(UIView*)v;
- (void)layoutSubviews; - (void)safeAreaInsetsDidChange; - (void)addSubview:(UIView*)v; - (void)removeFromSuperview;
- (void)bringSubviewToFront:(UIView*)v; - (UIView*)viewWithTag:(NSInteger)t;
- (CGSize)sizeThatFits:(CGSize)s;
@property CGFloat contentScaleFactor; @property (readonly) UIWindow *window; - (void)setNeedsLayout; - (void)layoutIfNeeded;
- (void)addGestureRecognizer:(UIGestureRecognizer*)g;
@end
@interface UILabel : UIView
@property (copy) NSString *text; @property (strong) UIColor *textColor; @property (strong) UIFont *font;
@property NSInteger textAlignment, numberOfLines; @property BOOL adjustsFontSizeToFitWidth; @property CGFloat minimumScaleFactor;
@end
@interface UIControl : UIView
@property (getter=isEnabled) BOOL enabled; @property (readonly, getter=isTracking) BOOL tracking;
- (void)addTarget:(id)t action:(SEL)a forControlEvents:(NSUInteger)e;
@end
@interface UISwitch : UIControl @property (getter=isOn) BOOL on; @end
@interface UISlider : UIControl
@property float minimumValue, maximumValue, value; @property (getter=isContinuous) BOOL continuous;
@end
@interface UISegmentedControl : UIControl
- (instancetype)initWithItems:(NSArray*)i; @property NSInteger selectedSegmentIndex;
@end
@interface UIButton : UIControl
+ (instancetype)buttonWithType:(NSInteger)t;
- (void)setTitle:(NSString*)t forState:(NSInteger)s; - (void)setTitleColor:(UIColor*)c forState:(NSInteger)s; - (void)setImage:(UIImage*)i forState:(NSInteger)s;
@property (readonly) UILabel *titleLabel; @property (strong) UIColor *tintColor;
@end
@interface UIScrollView : UIView
@property CGSize contentSize; @property CGPoint contentOffset;
@property NSInteger contentInsetAdjustmentBehavior; @property BOOL showsVerticalScrollIndicator;
- (void)flashScrollIndicators;
@end
@interface UIScene : NSObject @property (readonly) UIInterfaceOrientation interfaceOrientation; @property (readonly) NSInteger activationState; @end
@interface UIWindowScene : UIScene @property (readonly) UIInterfaceOrientation interfaceOrientation; @end
@interface UIViewController : NSObject
@property (nonatomic, strong) UIView *view; @property NSInteger overrideUserInterfaceStyle;
- (void)viewDidLoad; - (void)viewDidLayoutSubviews;
- (UIInterfaceOrientationMask)supportedInterfaceOrientations; - (BOOL)shouldAutorotate;
@end
@interface UIWindow : UIView
@property (readonly) UIScreen *screen;
@property (readonly) UIWindowScene *windowScene; @property CGFloat windowLevel;
@property (nonatomic, strong) UIViewController *rootViewController;
- (instancetype)initWithWindowScene:(UIWindowScene*)s; - (void)makeKeyAndVisible;
@end
@interface UIApplication : NSObject
+ (UIApplication*)sharedApplication; @property (readonly) NSSet *connectedScenes;
@end
@interface UIScreen : NSObject + (UIScreen*)mainScreen; @property (readonly) CGFloat scale, nativeScale; @property (readonly) CGRect bounds; @end
@interface UIImpactFeedbackGenerator : NSObject
- (instancetype)initWithStyle:(UIImpactFeedbackStyle)s; - (void)impactOccurred; - (void)prepare;
@end
@interface CMDeviceMotion : NSObject @property (readonly) struct { double x,y,z; } gravity; @end
@interface CMMotionManager : NSObject
@property (readonly) BOOL deviceMotionAvailable, deviceMotionActive; @property double deviceMotionUpdateInterval;
- (void)startDeviceMotionUpdatesToQueue:(NSOperationQueue*)q withHandler:(void (^)(CMDeviceMotion*, NSError*))h;
- (void)stopDeviceMotionUpdates;
@end
#endif
