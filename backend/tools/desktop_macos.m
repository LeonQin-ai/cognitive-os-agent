/* Native macOS host. The backend and user data live outside the app bundle. */
#import <Cocoa/Cocoa.h>
#import <WebKit/WebKit.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <signal.h>

@interface AppDelegate : NSObject <NSApplicationDelegate, WKNavigationDelegate>
@property NSWindow *window;
@property WKWebView *web;
@property NSTask *server;
@property NSURL *url;
@property NSDate *deadline;
@property BOOL closing;
@end

@implementation AppDelegate
- (void)showError:(NSString *)message {
    NSAlert *alert = [NSAlert new];
    alert.messageText = @"Cognitive OS 无法启动";
    alert.informativeText = message;
    [alert addButtonWithTitle:@"退出"];
    [alert runModal];
    [NSApp terminate:nil];
}
- (void)applicationDidFinishLaunching:(NSNotification *)note {
    (void)note;
    NSMenu *menu = [NSMenu new];
    NSMenuItem *app = [NSMenuItem new]; [menu addItem:app];
    NSMenu *appMenu = [NSMenu new];
    [appMenu addItemWithTitle:@"退出 Cognitive OS" action:@selector(terminate:) keyEquivalent:@"q"];
    app.submenu = appMenu;
    NSMenuItem *edit = [NSMenuItem new]; edit.title = @"编辑"; [menu addItem:edit];
    NSMenu *editMenu = [NSMenu new];
    [editMenu addItemWithTitle:@"撤销" action:@selector(undo:) keyEquivalent:@"z"];
    [editMenu addItemWithTitle:@"剪切" action:@selector(cut:) keyEquivalent:@"x"];
    [editMenu addItemWithTitle:@"复制" action:@selector(copy:) keyEquivalent:@"c"];
    [editMenu addItemWithTitle:@"粘贴" action:@selector(paste:) keyEquivalent:@"v"];
    [editMenu addItemWithTitle:@"全选" action:@selector(selectAll:) keyEquivalent:@"a"];
    edit.submenu = editMenu;
    NSApp.mainMenu = menu;

    self.window = [[NSWindow alloc] initWithContentRect:NSMakeRect(0,0,1180,800)
        styleMask:NSWindowStyleMaskTitled|NSWindowStyleMaskClosable|NSWindowStyleMaskMiniaturizable|NSWindowStyleMaskResizable
        backing:NSBackingStoreBuffered defer:NO];
    self.window.title = @"Cognitive OS"; self.window.minSize = NSMakeSize(640,480);
    self.web = [[WKWebView alloc] initWithFrame:self.window.contentView.bounds];
    self.web.autoresizingMask = NSViewWidthSizable|NSViewHeightSizable;
    self.web.navigationDelegate = self;
    self.window.contentView = self.web;
    [self.window center]; [self.window makeKeyAndOrderFront:nil]; [NSApp activateIgnoringOtherApps:YES];
    [self.web loadHTMLString:@"<meta charset='utf-8'><body style='background:#212121;color:#ececec;font:16px -apple-system;display:grid;place-items:center;height:90vh'><div><h2>Cognitive OS</h2><p>正在准备你的工作空间…</p></div>" baseURL:nil];

    NSFileManager *fm = NSFileManager.defaultManager;
    NSURL *support = [[fm URLsForDirectory:NSApplicationSupportDirectory inDomains:NSUserDomainMask].firstObject URLByAppendingPathComponent:@"Cognitive OS" isDirectory:YES];
    NSError *error = nil;
    if (![fm createDirectoryAtURL:support withIntermediateDirectories:YES attributes:nil error:&error]) {
        [self showError:error.localizedDescription]; return;
    }
    NSURL *log = [support URLByAppendingPathComponent:@"server.log"];
    if (![fm fileExistsAtPath:log.path]) [fm createFileAtPath:log.path contents:nil attributes:nil];
    NSFileHandle *output = [NSFileHandle fileHandleForWritingAtPath:log.path];
    [output seekToEndOfFile];

    /* Choose a free local port. If another process wins the short bind race,
       the child exits and readiness never navigates to that unrelated service. */
    int fd = socket(AF_INET,SOCK_STREAM,0);
    struct sockaddr_in addr = {0}; addr.sin_family=AF_INET; addr.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
    socklen_t length=sizeof(addr);
    if(fd<0 || bind(fd,(struct sockaddr *)&addr,sizeof(addr)) || getsockname(fd,(struct sockaddr *)&addr,&length)) {
        if(fd>=0) close(fd); [self showError:@"无法分配本地端口。"]; return;
    }
    unsigned port=ntohs(addr.sin_port); close(fd);
    self.url=[NSURL URLWithString:[NSString stringWithFormat:@"http://127.0.0.1:%u/",port]];
    self.server=[NSTask new];
    self.server.executableURL=[NSBundle.mainBundle URLForAuxiliaryExecutable:@"cognitive-os-agent"];
    self.server.arguments=@[@"serve", [NSString stringWithFormat:@"%u",port]];
    self.server.currentDirectoryURL=support;
    self.server.standardOutput=output; self.server.standardError=output;
    if(!self.server.executableURL || ![self.server launchAndReturnError:&error]) {
        [self showError:error.localizedDescription ?: @"应用缺少后端程序。"]; return;
    }
    self.deadline=[NSDate dateWithTimeIntervalSinceNow:45];
    [self checkReady];
}
- (void)checkReady {
    if(self.closing) return;
    if(!self.server.running || self.deadline.timeIntervalSinceNow<=0) {
        [self showError:@"本地服务未就绪。请查看 ~/Library/Application Support/Cognitive OS/server.log。"]; return;
    }
    NSMutableURLRequest *request=[NSMutableURLRequest requestWithURL:self.url]; request.timeoutInterval=1;
    [[NSURLSession.sharedSession dataTaskWithRequest:request completionHandler:^(NSData *data, NSURLResponse *response, NSError *error) {
        dispatch_async(dispatch_get_main_queue(), ^{
            if(self.closing) return;
            NSString *html=data ? [[NSString alloc] initWithData:data encoding:NSUTF8StringEncoding] : nil;
            if(!error && [(NSHTTPURLResponse *)response statusCode]==200 && self.server.running && [html containsString:@"Cognitive OS"])
                [self.web loadRequest:[NSURLRequest requestWithURL:self.url]];
            else dispatch_after(dispatch_time(DISPATCH_TIME_NOW,500*NSEC_PER_MSEC),dispatch_get_main_queue(),^{ [self checkReady]; });
        });
    }] resume];
}
- (void)webView:(WKWebView *)webView decidePolicyForNavigationAction:(WKNavigationAction *)action decisionHandler:(void (^)(WKNavigationActionPolicy))handler {
    (void)webView;
    NSURL *url=action.request.URL;
    if([url.scheme isEqualToString:@"about"] || ([url.host isEqualToString:self.url.host] && [url.port isEqual:self.url.port] && [url.scheme isEqualToString:@"http"])) {
        handler(WKNavigationActionPolicyAllow); return;
    }
    if(action.navigationType==WKNavigationTypeLinkActivated && ( [url.scheme isEqualToString:@"https"] || [url.scheme isEqualToString:@"http"]))
        [NSWorkspace.sharedWorkspace openURL:url];
    handler(WKNavigationActionPolicyCancel);
}
- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication *)sender { (void)sender; return YES; }
- (void)applicationWillTerminate:(NSNotification *)note {
    (void)note; self.closing=YES;
    if(self.server.running) {
        [self.server terminate];
        for(int i=0;i<20 && self.server.running;i++) [NSThread sleepForTimeInterval:0.05];
        if(self.server.running) kill(self.server.processIdentifier,SIGKILL);
        [self.server waitUntilExit];
    }
}
@end
int main(int argc,const char **argv) {
    (void)argc; (void)argv;
    @autoreleasepool {
        NSApplication *app=NSApplication.sharedApplication;
        AppDelegate *delegate=[AppDelegate new]; app.delegate=delegate;
        [app setActivationPolicy:NSApplicationActivationPolicyRegular]; [app run];
    }
    return 0;
}
