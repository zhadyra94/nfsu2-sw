/* ios_keyboard.m -- is a hardware keyboard attached? (iOS 14+ GameController)
 *
 * Only the presence is tracked here. The keys themselves are read from SDL's
 * own key state (SDL_GetKeyboardState, see xbox_ios_keyboard_mask_from_sdl in
 * xinput_ios.c): SDL's UIKit backend owns GCKeyboard's keyChangedHandler, so
 * installing a second one would fight it. The game thread reads the atomic
 * flag below and never touches a GameController object.
 *
 * No Win32 header here (its BOOL collides with ObjC's): plain C types only. */
#import <Foundation/Foundation.h>
#import <GameController/GameController.h>
#include <stdio.h>

static int s_connected;   /* __atomic: written on the main queue, read by the game thread */
static int s_installed;

/* Main thread. Safe to call twice. */
void ios_keyboard_install(void)
{
    NSNotificationCenter *nc;

    if (s_installed)
        return;
    s_installed = 1;
    nc = [NSNotificationCenter defaultCenter];
    [nc addObserverForName:GCKeyboardDidConnectNotification
                    object:nil
                     queue:[NSOperationQueue mainQueue]
                usingBlock:^(NSNotification *note) {
                    (void)note;
                    __atomic_store_n(&s_connected, 1, __ATOMIC_RELAXED);
                    fprintf(stderr, "nfsu2: keyboard connected\n");
                }];
    [nc addObserverForName:GCKeyboardDidDisconnectNotification
                    object:nil
                     queue:[NSOperationQueue mainQueue]
                usingBlock:^(NSNotification *note) {
                    /* Another keyboard may still be there. */
                    GCKeyboard *left = [GCKeyboard coalescedKeyboard];
                    int still = (left && (id)left != note.object) ? 1 : 0;
                    __atomic_store_n(&s_connected, still, __ATOMIC_RELAXED);
                    fprintf(stderr, "nfsu2: keyboard disconnected\n");
                }];
    if ([GCKeyboard coalescedKeyboard]) {
        __atomic_store_n(&s_connected, 1, __ATOMIC_RELAXED);
        fprintf(stderr, "nfsu2: keyboard present at start\n");
    }
}

/* Game thread: an atomic read only. */
int ios_keyboard_connected(void)
{
    return __atomic_load_n(&s_connected, __ATOMIC_RELAXED);
}
