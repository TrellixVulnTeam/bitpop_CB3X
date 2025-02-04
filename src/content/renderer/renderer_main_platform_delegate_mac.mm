// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "content/renderer/renderer_main_platform_delegate.h"

#include <Carbon/Carbon.h>
#import <Cocoa/Cocoa.h>
#include <objc/runtime.h>

#include "base/command_line.h"
#include "base/logging.h"
#include "base/mac/mac_util.h"
#include "base/mac/scoped_cftyperef.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/sys_string_conversions.h"
#include "content/common/sandbox_mac.h"
#include "content/public/common/content_switches.h"
#import "content/public/common/injection_test_mac.h"
#include "content/common/sandbox_init_mac.h"

namespace content {

namespace {

// You are about to read a pretty disgusting hack. In a static initializer,
// CoreFoundation decides to connect with cfprefsd(8) using Mach IPC. There is
// no public way to close this Mach port after-the-fact, nor a way to stop it
// from happening since it is done pre-main in dyld. But the address of the
// CFMachPort can be found in the run loop's string description. Below, that
// address is parsed, cast, and then used to invalidate the Mach port to
// disable communication with cfprefsd.
void DisconnectCFNotificationCenter() {
  base::ScopedCFTypeRef<CFStringRef> run_loop_description(
      CFCopyDescription(CFRunLoopGetCurrent()));
  const CFIndex length = CFStringGetLength(run_loop_description);
  for (CFIndex i = 0; i < length; ) {
    // Find the start of a CFMachPort run loop source, which looks like this,
    // without new lines:
    // 1 : <CFRunLoopSource 0x7d16ea90 [0xa160af80]>{signalled = No,
    // valid = Yes, order = 0, context =
    // <CFMachPort 0x7d16fe00 [0xa160af80]>{valid = Yes, port = 3a0f,
    // source = 0x7d16ea90, callout =
    // _ZL14MessageHandlerP12__CFMachPortPvlS1_ (0x96df59c2), context =
    // <CFMachPort context 0x1475b>}}
    CFRange run_loop_source_context_range;
    if (!CFStringFindWithOptions(run_loop_description,
            CFSTR(", context = <CFMachPort "), CFRangeMake(i, length - i),
            0, &run_loop_source_context_range)) {
      break;
    }
    i = run_loop_source_context_range.location +
        run_loop_source_context_range.length;

    // The address of the CFMachPort is the first hexadecimal address after the
    // CF type name.
    CFRange port_address_range = CFRangeMake(i, 0);
    for (CFIndex j = port_address_range.location; j < length; ++j) {
      UniChar c = CFStringGetCharacterAtIndex(run_loop_description, j);
      if (c == ' ')
        break;
      ++port_address_range.length;
    }

    base::ScopedCFTypeRef<CFStringRef> port_address_string(
        CFStringCreateWithSubstring(NULL, run_loop_description,
            port_address_range));
    if (!port_address_string)
      continue;

    // Convert the string to an address.
    std::string port_address_std_string =
        base::SysCFStringRefToUTF8(port_address_string);
#if __LP64__
    uint64 port_address = 0;
    if (!base::HexStringToUInt64(port_address_std_string, &port_address))
      continue;
#else
    uint32 port_address = 0;
    if (!base::HexStringToUInt(port_address_std_string, &port_address))
      continue;
#endif

    // Cast the address to an object.
    CFMachPortRef mach_port = reinterpret_cast<CFMachPortRef>(port_address);
    if (CFGetTypeID(mach_port) != CFMachPortGetTypeID())
      continue;

    // Verify that this is the Mach port that needs to be disconnected by the
    // name of its callout function. Example description (no new lines):
    // <CFMachPort 0x7d16fe00 [0xa160af80]>{valid = Yes, port = 3a0f, source =
    // 0x7d16ea90, callout = __CFXNotificationReceiveFromServer (0x96df59c2),
    // context = <CFMachPort context 0x1475b>}
    base::ScopedCFTypeRef<CFStringRef> port_description(
        CFCopyDescription(mach_port));
    if (CFStringFindWithOptions(port_description,
            CFSTR(", callout = __CFXNotificationReceiveFromServer ("),
            CFRangeMake(0, CFStringGetLength(port_description)),
            0,
            NULL)) {
      CFMachPortInvalidate(mach_port);
      return;
    }
  }
}

}  // namespace

RendererMainPlatformDelegate::RendererMainPlatformDelegate(
    const MainFunctionParams& parameters)
        : parameters_(parameters) {
}

RendererMainPlatformDelegate::~RendererMainPlatformDelegate() {
}

// TODO(mac-port): Any code needed to initialize a process for purposes of
// running a renderer needs to also be reflected in chrome_main.cc for
// --single-process support.
void RendererMainPlatformDelegate::PlatformInitialize() {
  if (base::mac::IsOSYosemiteOrLater()) {
    // This is needed by the NSAnimations run for the scrollbars. If we switch
    // from native scrollers to drawing them in some other way, this warmup can
    // be removed <http://crbug.com/306348>.
    [NSScreen screens];
  }

  if (![NSThread isMultiThreaded]) {
    NSString* string = @"";
    [NSThread detachNewThreadSelector:@selector(length)
                             toTarget:string
                           withObject:nil];
  }
}

void RendererMainPlatformDelegate::PlatformUninitialize() {
}

static void LogTestMessage(std::string message, bool is_error) {
  if (is_error)
    LOG(ERROR) << message;
  else
    VLOG(0) << message;
}

bool RendererMainPlatformDelegate::InitSandboxTests(bool no_sandbox) {
  const CommandLine& command_line = parameters_.command_line;

  if (command_line.HasSwitch(switches::kTestSandbox)) {
    std::string bundle_path =
    command_line.GetSwitchValueNative(switches::kTestSandbox);
    if (bundle_path.empty()) {
      NOTREACHED() << "Bad bundle path";
      return false;
    }
    NSBundle* tests_bundle =
        [NSBundle bundleWithPath:base::SysUTF8ToNSString(bundle_path)];
    if (![tests_bundle load]) {
      NOTREACHED() << "Failed to load bundle";
      return false;
    }
    sandbox_tests_bundle_ = [tests_bundle retain];
    [objc_getClass("RendererSandboxTestsRunner") setLogFunction:LogTestMessage];
  }
  return true;
}

bool RendererMainPlatformDelegate::EnableSandbox() {
  // Enable the sandbox.
  bool sandbox_initialized = InitializeSandbox();

  // The sandbox is now engaged. Make sure that the renderer has not connected
  // itself to Cocoa.
  CHECK(NSApp == nil);

  DisconnectCFNotificationCenter();

  return sandbox_initialized;
}

void RendererMainPlatformDelegate::RunSandboxTests(bool no_sandbox) {
  Class tests_runner = objc_getClass("RendererSandboxTestsRunner");
  if (tests_runner) {
    if (![tests_runner runTests])
      LOG(ERROR) << "Running renderer with failing sandbox tests!";
    [sandbox_tests_bundle_ unload];
    [sandbox_tests_bundle_ release];
    sandbox_tests_bundle_ = nil;
  }
}

}  // namespace content
