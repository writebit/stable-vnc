#import <Cocoa/Cocoa.h>
#include "vnc_server.h"
#include "network_info.h"
#include "logging.h"

@interface AppDelegate : NSObject <NSApplicationDelegate, NSTextFieldDelegate>
@property (nonatomic) NSStatusItem *statusItem;
@property (nonatomic) NSWindow *statusWindow;
@property (nonatomic) NSTextField *ipv4Label;
@property (nonatomic) NSTextField *ipv6Label;
@property (nonatomic) NSTextField *portField;
@property (nonatomic) NSSecureTextField *passwordField;
@property (nonatomic) NSTextField *statusLabel;
@property (nonatomic) NSTextField *clientCountLabel;
@property (nonatomic) NSTextField *logPathLabel;
@property (nonatomic) NSButton *toggleButton;
@property (nonatomic) VNCServer server;
@property (nonatomic) BOOL serverRunning;
@property (nonatomic) NSTimer *refreshTimer;
@end

@implementation AppDelegate

- (void)applicationDidFinishLaunching:(NSNotification *)notification {
    self.serverRunning = NO;

    // Initialize file logging
    log_init(NULL); // defaults to ~/Library/Logs/StableVNC.log
    LOG_INFO("App", "StableVNC application launched");

    [self createMenuBarIcon];
    [self createStatusWindow];

    vnc_server_set_status_callback(status_callback);

    self.refreshTimer = [NSTimer scheduledTimerWithTimeInterval:5.0
                                                         target:self
                                                       selector:@selector(refreshNetworkInfo)
                                                       userInfo:nil
                                                        repeats:YES];

    [self refreshNetworkInfo];
}

static void status_callback(int client_count, bool running) {
    dispatch_async(dispatch_get_main_queue(), ^{
        AppDelegate *delegate = (AppDelegate *)[NSApp delegate];
        [delegate updateStatusUI:client_count running:running];
    });
}

- (void)createMenuBarIcon {
    self.statusItem = [[NSStatusBar systemStatusBar] statusItemWithLength:NSVariableStatusItemLength];

    NSImage *icon = [self createStatusBarIcon];
    icon.template = YES;
    self.statusItem.button.image = icon;
    self.statusItem.button.toolTip = @"StableVNC Server";
    self.statusItem.button.target = self;
    self.statusItem.button.action = @selector(statusItemClicked:);

    NSMenu *menu = [[NSMenu alloc] init];
    [menu addItemWithTitle:@"Show StableVNC" action:@selector(showStatusWindow:) keyEquivalent:@""];
    [menu addItem:[NSMenuItem separatorItem]];
    [menu addItemWithTitle:@"Start Server" action:@selector(toggleServer:) keyEquivalent:@""];
    [menu addItem:[NSMenuItem separatorItem]];
    [menu addItemWithTitle:@"Open Log File" action:@selector(openLogFile:) keyEquivalent:@""];
    [menu addItem:[NSMenuItem separatorItem]];
    [menu addItemWithTitle:@"Quit StableVNC" action:@selector(quitApp:) keyEquivalent:@"q"];

    for (NSMenuItem *item in menu.itemArray) {
        item.target = self;
    }

    self.statusItem.menu = menu;
}

- (NSImage *)createStatusBarIcon {
    NSImage *image = [[NSImage alloc] initWithSize:NSMakeSize(18, 18)];
    [image lockFocus];

    // Draw a simple monitor icon
    NSBezierPath *monitor = [NSBezierPath bezierPathWithRoundedRect:NSMakeRect(1, 4, 16, 12)
                                                            xRadius:1.5
                                                            yRadius:1.5];
    [[NSColor labelColor] setStroke];
    monitor.lineWidth = 1.2;
    [monitor stroke];

    // Screen area
    [[NSColor labelColor] setFill];
    NSRectFill(NSMakeRect(3, 6, 12, 8));

    // Stand
    NSBezierPath *stand = [NSBezierPath bezierPath];
    [stand moveToPoint:NSMakePoint(6, 4)];
    [stand lineToPoint:NSMakePoint(9, 1)];
    [stand lineToPoint:NSMakePoint(12, 4)];
    stand.lineWidth = 1.2;
    [stand stroke];

    // VNC text-like dots inside screen
    [[NSColor windowBackgroundColor] setFill];
    NSRectFill(NSMakeRect(5, 9, 2, 2));
    NSRectFill(NSMakeRect(8, 9, 2, 2));
    NSRectFill(NSMakeRect(11, 9, 2, 2));

    [image unlockFocus];
    return image;
}

- (void)createStatusWindow {
    NSRect frame = NSMakeRect(0, 0, 420, 480);
    NSWindowStyleMask style = NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                               NSWindowStyleMaskMiniaturizable;
    self.statusWindow = [[NSWindow alloc] initWithContentRect:frame
                                                     styleMask:style
                                                       backing:NSBackingStoreBuffered
                                                         defer:NO];
    self.statusWindow.title = @"StableVNC Server";
    self.statusWindow.releasedWhenClosed = NO;
    [self.statusWindow center];

    NSView *content = self.statusWindow.contentView;

    // Title
    NSTextField *title = [self createLabel:@"StableVNC Server" frame:NSMakeRect(20, 435, 380, 28)];
    title.font = [NSFont boldSystemFontOfSize:20];
    [content addSubview:title];

    // Separator
    NSBox *sep1 = [[NSBox alloc] initWithFrame:NSMakeRect(20, 425, 380, 1)];
    sep1.boxType = NSBoxSeparator;
    [content addSubview:sep1];

    // Status section
    NSTextField *statusTitle = [self createLabel:@"Server Status" frame:NSMakeRect(20, 397, 380, 20)];
    statusTitle.font = [NSFont boldSystemFontOfSize:14];
    [content addSubview:statusTitle];

    self.statusLabel = [self createLabel:@"● Stopped" frame:NSMakeRect(40, 374, 340, 20)];
    self.statusLabel.textColor = [NSColor systemRedColor];
    [content addSubview:self.statusLabel];

    self.clientCountLabel = [self createLabel:@"Connected clients: 0" frame:NSMakeRect(40, 352, 340, 20)];
    [content addSubview:self.clientCountLabel];

    // Network section
    NSBox *sep2 = [[NSBox alloc] initWithFrame:NSMakeRect(20, 339, 380, 1)];
    sep2.boxType = NSBoxSeparator;
    [content addSubview:sep2];

    NSTextField *netTitle = [self createLabel:@"Network Addresses" frame:NSMakeRect(20, 311, 380, 20)];
    netTitle.font = [NSFont boldSystemFontOfSize:14];
    [content addSubview:netTitle];

    NSTextField *ipv4Title = [self createLabel:@"IPv4:" frame:NSMakeRect(30, 287, 60, 20)];
    ipv4Title.font = [NSFont boldSystemFontOfSize:12];
    [content addSubview:ipv4Title];

    self.ipv4Label = [self createLabel:@"Detecting..." frame:NSMakeRect(90, 287, 300, 20)];
    self.ipv4Label.selectable = YES;
    [content addSubview:self.ipv4Label];

    NSTextField *ipv6Title = [self createLabel:@"IPv6:" frame:NSMakeRect(30, 262, 60, 20)];
    ipv6Title.font = [NSFont boldSystemFontOfSize:12];
    [content addSubview:ipv6Title];

    self.ipv6Label = [self createLabel:@"Detecting..." frame:NSMakeRect(90, 242, 300, 40)];
    self.ipv6Label.selectable = YES;
    self.ipv6Label.maximumNumberOfLines = 3;
    [content addSubview:self.ipv6Label];

    // Port section
    NSBox *sep3 = [[NSBox alloc] initWithFrame:NSMakeRect(20, 229, 380, 1)];
    sep3.boxType = NSBoxSeparator;
    [content addSubview:sep3];

    NSTextField *settingsTitle = [self createLabel:@"Connection Settings" frame:NSMakeRect(20, 201, 380, 20)];
    settingsTitle.font = [NSFont boldSystemFontOfSize:14];
    [content addSubview:settingsTitle];

    NSTextField *portLabel = [self createLabel:@"VNC Port:" frame:NSMakeRect(30, 174, 80, 22)];
    [content addSubview:portLabel];

    self.portField = [[NSTextField alloc] initWithFrame:NSMakeRect(110, 174, 100, 22)];
    self.portField.stringValue = @"5900";
    self.portField.delegate = self;
    self.portField.formatter = ({
        NSNumberFormatter *f = [[NSNumberFormatter alloc] init];
        f.minimum = @(1);
        f.maximum = @(65535);
        f.allowsFloats = NO;
        f;
    });
    [content addSubview:self.portField];

    NSTextField *portHint = [self createLabel:@"(1-65535, default: 5900)" frame:NSMakeRect(220, 174, 180, 22)];
    portHint.textColor = [NSColor secondaryLabelColor];
    portHint.font = [NSFont systemFontOfSize:11];
    [content addSubview:portHint];

    NSTextField *pwLabel = [self createLabel:@"Password:" frame:NSMakeRect(30, 146, 80, 22)];
    [content addSubview:pwLabel];

    self.passwordField = [[NSSecureTextField alloc] initWithFrame:NSMakeRect(110, 146, 160, 22)];
    self.passwordField.placeholderString = @"(none)";
    [content addSubview:self.passwordField];

    NSTextField *pwHint = [self createLabel:@"(empty = no auth)" frame:NSMakeRect(280, 146, 130, 22)];
    pwHint.textColor = [NSColor secondaryLabelColor];
    pwHint.font = [NSFont systemFontOfSize:11];
    [content addSubview:pwHint];

    // Log file section
    NSBox *sep4 = [[NSBox alloc] initWithFrame:NSMakeRect(20, 131, 380, 1)];
    sep4.boxType = NSBoxSeparator;
    [content addSubview:sep4];

    NSTextField *logTitle = [self createLabel:@"Log File" frame:NSMakeRect(20, 105, 60, 20)];
    logTitle.font = [NSFont boldSystemFontOfSize:12];
    [content addSubview:logTitle];

    self.logPathLabel = [self createLabel:@"" frame:NSMakeRect(85, 105, 270, 20)];
    self.logPathLabel.selectable = YES;
    self.logPathLabel.font = [NSFont systemFontOfSize:11];
    self.logPathLabel.textColor = [NSColor secondaryLabelColor];
    self.logPathLabel.stringValue = [NSString stringWithUTF8String:log_get_filepath()];
    [content addSubview:self.logPathLabel];

    NSButton *openLogButton = [[NSButton alloc] initWithFrame:NSMakeRect(360, 103, 40, 24)];
    openLogButton.title = @"Open";
    openLogButton.bezelStyle = NSBezelStyleRounded;
    openLogButton.font = [NSFont systemFontOfSize:10];
    openLogButton.target = self;
    openLogButton.action = @selector(openLogFile:);
    [content addSubview:openLogButton];

    // Toggle button
    self.toggleButton = [[NSButton alloc] initWithFrame:NSMakeRect(120, 20, 180, 36)];
    self.toggleButton.title = @"Start Server";
    self.toggleButton.bezelStyle = NSBezelStyleRounded;
    self.toggleButton.target = self;
    self.toggleButton.action = @selector(toggleServer:);
    self.toggleButton.font = [NSFont systemFontOfSize:14];
    [content addSubview:self.toggleButton];

    [self.statusWindow makeKeyAndOrderFront:nil];
}

- (NSTextField *)createLabel:(NSString *)text frame:(NSRect)frame {
    NSTextField *label = [[NSTextField alloc] initWithFrame:frame];
    label.stringValue = text;
    label.bordered = NO;
    label.editable = NO;
    label.drawsBackground = NO;
    label.font = [NSFont systemFontOfSize:13];
    return label;
}

- (void)refreshNetworkInfo {
    NetworkAddresses addrs;
    network_get_addresses(&addrs);

    NSMutableString *ipv4 = [NSMutableString string];
    if (addrs.ipv4_count == 0) {
        [ipv4 appendString:@"No IPv4 address found"];
    } else {
        for (int i = 0; i < addrs.ipv4_count; i++) {
            if (i > 0) [ipv4 appendString:@", "];
            [ipv4 appendFormat:@"%s", addrs.ipv4_addrs[i]];
        }
    }
    self.ipv4Label.stringValue = ipv4;

    NSMutableString *ipv6 = [NSMutableString string];
    if (addrs.ipv6_count == 0) {
        [ipv6 appendString:@"No IPv6 address found"];
    } else {
        for (int i = 0; i < addrs.ipv6_count; i++) {
            if (i > 0) [ipv6 appendString:@"\n"];
            [ipv6 appendFormat:@"%s", addrs.ipv6_addrs[i]];
        }
    }
    self.ipv6Label.stringValue = ipv6;
}

- (void)toggleServer:(id)sender {
    if (self.serverRunning) {
        LOG_INFO("App", "User stopping server");
        vnc_server_stop(&_server);
        self.serverRunning = NO;
        self.toggleButton.title = @"Start Server";
        self.portField.enabled = YES;
        self.statusLabel.stringValue = @"● Stopped";
        self.statusLabel.textColor = [NSColor systemRedColor];
        self.clientCountLabel.stringValue = @"Connected clients: 0";

        // Update menu
        NSMenuItem *item = [self.statusItem.menu itemAtIndex:2];
        item.title = @"Start Server";
    } else {
        uint16_t port = (uint16_t)self.portField.integerValue;
        if (port == 0) port = VNC_DEFAULT_PORT;
        NSString *password = self.passwordField.stringValue ?: @"";

        LOG_INFO("App", "User starting server on port %d", port);

        if (!vnc_server_init(&_server, port)) {
            LOG_ERROR("App", "Failed to initialize VNC server");
            NSAlert *alert = [[NSAlert alloc] init];
            alert.messageText = @"Failed to initialize VNC server";
            alert.informativeText = @"Could not initialize screen capture. Make sure Screen Recording permission is granted in System Settings > Privacy & Security.";
            alert.alertStyle = NSAlertStyleCritical;
            [alert runModal];
            return;
        }

        vnc_server_set_password(&_server, password.UTF8String);

        if (!vnc_server_start(&_server)) {
            LOG_ERROR("App", "Failed to start server on port %d", port);
            NSAlert *alert = [[NSAlert alloc] init];
            alert.messageText = @"Failed to start VNC server";
            alert.informativeText = [NSString stringWithFormat:@"Could not bind to port %d. The port may be in use.", port];
            alert.alertStyle = NSAlertStyleCritical;
            [alert runModal];
            vnc_server_cleanup(&_server);
            return;
        }

        self.serverRunning = YES;
        self.toggleButton.title = @"Stop Server";
        self.portField.enabled = NO;
        self.statusLabel.stringValue = [NSString stringWithFormat:@"● Running on port %d", port];
        self.statusLabel.textColor = [NSColor systemGreenColor];

        NSMenuItem *item = [self.statusItem.menu itemAtIndex:2];
        item.title = @"Stop Server";
    }
}

- (void)updateStatusUI:(int)clientCount running:(BOOL)running {
    self.clientCountLabel.stringValue = [NSString stringWithFormat:@"Connected clients: %d", clientCount];

    if (running) {
        self.statusItem.button.image = [self createActiveStatusBarIcon];
        self.statusItem.button.image.template = YES;
    } else {
        NSImage *icon = [self createStatusBarIcon];
        icon.template = YES;
        self.statusItem.button.image = icon;
    }
}

- (NSImage *)createActiveStatusBarIcon {
    NSImage *image = [self createStatusBarIcon];
    [image lockFocus];
    [[NSColor systemGreenColor] setFill];
    NSBezierPath *dot = [NSBezierPath bezierPathWithOvalInRect:NSMakeRect(13, 1, 5, 5)];
    [dot fill];
    [image unlockFocus];
    return image;
}

- (void)statusItemClicked:(id)sender {
    [self showStatusWindow:sender];
}

- (void)showStatusWindow:(id)sender {
    [self.statusWindow makeKeyAndOrderFront:nil];
    [NSApp activateIgnoringOtherApps:YES];
    [self refreshNetworkInfo];
}

- (void)openLogFile:(id)sender {
    NSString *logPath = [NSString stringWithUTF8String:log_get_filepath()];
    NSURL *fileURL = [NSURL fileURLWithPath:logPath];
    NSURL *consoleURL = [[NSWorkspace sharedWorkspace] URLForApplicationWithBundleIdentifier:@"com.apple.Console"];
    if (consoleURL) {
        NSWorkspaceOpenConfiguration *config = [NSWorkspaceOpenConfiguration configuration];
        [[NSWorkspace sharedWorkspace] openURLs:@[fileURL]
                           withApplicationAtURL:consoleURL
                                  configuration:config
                              completionHandler:nil];
    } else {
        [[NSWorkspace sharedWorkspace] openURL:fileURL];
    }
}

- (void)quitApp:(id)sender {
    LOG_INFO("App", "StableVNC quitting");
    if (self.serverRunning) {
        vnc_server_cleanup(&_server);
    }
    [self.refreshTimer invalidate];
    log_close();
    [NSApp terminate:nil];
}

- (void)applicationWillTerminate:(NSNotification *)notification {
    if (self.serverRunning) {
        vnc_server_cleanup(&_server);
    }
    log_close();
}

- (BOOL)applicationShouldHandleReopen:(NSApplication *)sender hasVisibleWindows:(BOOL)flag {
    [self showStatusWindow:nil];
    return YES;
}

@end

int main(int argc __attribute__((unused)), const char *argv[] __attribute__((unused))) {
    @autoreleasepool {
        NSApplication *app = [NSApplication sharedApplication];
        [app setActivationPolicy:NSApplicationActivationPolicyAccessory];

        AppDelegate *delegate = [[AppDelegate alloc] init];
        app.delegate = delegate;

        [app run];
    }
    return 0;
}
