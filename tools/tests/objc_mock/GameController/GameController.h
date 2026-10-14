/* Mock GameController subset used by ios_keyboard.m (iOS 14 API shapes). */
extern NSString *const GCKeyboardDidConnectNotification;
extern NSString *const GCKeyboardDidDisconnectNotification;
@interface GCKeyboard : NSObject
@property (class, readonly) GCKeyboard *coalescedKeyboard;
@end
