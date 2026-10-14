/* Mock: everything lives in Mock.h (force-included). */
@interface NSNotification : NSObject
@property (readonly) id object;
@property (readonly) NSString *name;
@end
@interface NSNotificationCenter : NSObject
+ (NSNotificationCenter *)defaultCenter;
- (id)addObserverForName:(NSString *)name object:(id)obj queue:(NSOperationQueue *)queue
              usingBlock:(void (^)(NSNotification *note))block;
@end
