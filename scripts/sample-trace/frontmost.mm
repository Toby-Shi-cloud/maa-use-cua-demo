#import <AppKit/AppKit.h>
#include <cstdio>

// Read-only probe: the experiment runner rejects a foreground Sample trial.
int main() {
    @autoreleasepool {
        NSRunningApplication* app = NSWorkspace.sharedWorkspace.frontmostApplication;
        printf("%d\t%s\n", app.processIdentifier, app.localizedName.UTF8String);
    }
}
