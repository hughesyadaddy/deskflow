/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2015 Synergy App Ltd
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#import "OSXHelpers.h"

#import <ApplicationServices/ApplicationServices.h>
#import <Cocoa/Cocoa.h>
#import <CoreData/CoreData.h>
#import <ServiceManagement/ServiceManagement.h>
#import <Foundation/Foundation.h>
#import <UserNotifications/UNNotification.h>
#import <UserNotifications/UNNotificationContent.h>
#import <UserNotifications/UNNotificationTrigger.h>
#import <UserNotifications/UNUserNotificationCenter.h>

#import <QCoreApplication>
#import <QDir>
#import <QFile>
#import <QFileInfo>
#import <QtGlobal>

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"

void requestOSXNotificationPermission()
{
#if OSX_DEPLOYMENT_TARGET >= 1014
  if (isOSXDevelopmentBuild()) {
    qWarning("Not requesting notification permission in dev build");
    return;
  }

  UNUserNotificationCenter *center = [UNUserNotificationCenter currentNotificationCenter];
  [center requestAuthorizationWithOptions:(UNAuthorizationOptionAlert + UNAuthorizationOptionSound)
                        completionHandler:^(BOOL granted, NSError *_Nullable error) {
                          if (error != nil) {
                            qWarning(
                                "Notification permission request error: %s",
                                [[NSString stringWithFormat:@"%@", error] UTF8String]
                            );
                          }
                        }];
#endif
}

bool isOSXDevelopmentBuild()
{
  std::string bundleURL = [[[NSBundle mainBundle] bundleURL].absoluteString UTF8String];
  return (bundleURL.find("Applications/Deskflow.app") == std::string::npos);
}

bool showOSXNotification(const QString &title, const QString &body)
{
#if OSX_DEPLOYMENT_TARGET >= 1014
  // accessing notification center on unsigned build causes an immidiate
  // application shutodown (in this case, server) and cannot be caught
  // to avoid issues with it need to first check if this is a dev build
  if (isOSXDevelopmentBuild()) {
    qWarning("Not showing notification in dev build");
    return false;
  }

  requestOSXNotificationPermission();

  UNUserNotificationCenter *center = [UNUserNotificationCenter currentNotificationCenter];

  UNMutableNotificationContent *content = [[UNMutableNotificationContent alloc] init];
  content.title = title.toNSString();
  content.body = body.toNSString();

  // Create the request object.
  UNNotificationRequest *request = [UNNotificationRequest requestWithIdentifier:@"SecureInput"
                                                                        content:content
                                                                        trigger:nil];

  [center
      addNotificationRequest:request
       withCompletionHandler:^(NSError *_Nullable error) {
         if (error != nil) {
           qWarning("Notification display request error: %s", [[NSString stringWithFormat:@"%@", error] UTF8String]);
         }
       }];
#else
  NSUserNotification *notification = [[NSUserNotification alloc] init];
  notification.title = title.toNSString();
  notification.informativeText = body.toNSString();
  notification.soundName = NSUserNotificationDefaultSoundName; // Will play a default sound
  [[NSUserNotificationCenter defaultUserNotificationCenter] deliverNotification:notification];
  [notification autorelease];
#endif
  return true;
}

bool isOSXInterfaceStyleDark()
{
  // Implementation from http://stackoverflow.com/a/26472651
  NSDictionary *dict = [[NSUserDefaults standardUserDefaults] persistentDomainForName:NSGlobalDomain];
  id style = [dict objectForKey:@"AppleInterfaceStyle"];
  return (style && [style isKindOfClass:[NSString class]] && NSOrderedSame == [style caseInsensitiveCompare:@"dark"]);
}

void macOSSetDockVisible(bool visible)
{
  const auto policy =
      visible ? NSApplicationActivationPolicyRegular : NSApplicationActivationPolicyAccessory;
  [[NSApplication sharedApplication] setActivationPolicy:policy];
  if (visible) {
    [[NSApplication sharedApplication] activateIgnoringOtherApps:YES];
  }
}

bool macOSIsDockVisible()
{
  return [[NSApplication sharedApplication] activationPolicy] == NSApplicationActivationPolicyRegular;
}

void forceAppActive()
{
  macOSSetDockVisible(true);
}

void macOSNativeHide()
{
  macOSSetDockVisible(false);
  [NSApp hide:nil];
}

bool macSetStartAtLogin(bool enable)
{
  if (@available(macOS 13.0, *)) {
    SMAppService *service = [SMAppService mainAppService];
    NSError *error = nil;
    const BOOL ok = enable ? [service registerAndReturnError:&error] : [service unregisterAndReturnError:&error];
    if (!ok && error != nil) {
      NSLog(@"Deskflow start-at-login %@ failed: %@", enable ? @"register" : @"unregister", error);
    }
    return ok;
  }
  return false;
}

bool macStartAtLoginEnabled()
{
  if (@available(macOS 13.0, *)) {
    return [SMAppService mainAppService].status == SMAppServiceStatusEnabled;
  }
  return false;
}

namespace {
bool launchAgentInstalled(const char *label)
{
  return QFile::exists(QDir::homePath() + QStringLiteral("/Library/LaunchAgents/%1.plist").arg(QLatin1String(label)));
}
} // namespace

bool macLaunchdOwnsGui()
{
  // Environment only, never the plist: the plist exists for every copy of the
  // GUI on the box (the LaunchAgent one AND a BTM/Login Item one), so a file
  // test made the unmanaged incumbent claim "launchd owns me", refuse the
  // handoff, and leave launchd's copy exiting 5 at every login.
  return qEnvironmentVariable("DESKFLOW_LAUNCHD") == QLatin1String("1");
}

bool macGuiLaunchAgentInstalled()
{
  return launchAgentInstalled("io.github.hughesyadaddy.deskflow");
}

bool macLaunchdOwnsCore()
{
  return launchAgentInstalled("io.github.hughesyadaddy.deskflow-core");
}

QString macQuitIntentPath()
{
  return QDir::homePath() + QStringLiteral("/Library/Application Support/Deskflow/quit-intent");
}

bool macWriteQuitIntent()
{
  const auto path = macQuitIntentPath();
  if (!QDir().mkpath(QFileInfo(path).absolutePath())) {
    qWarning("could not create the quit-intent directory for %s", qPrintable(path));
    return false;
  }
  QFile sentinel(path);
  if (!sentinel.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
    qWarning("could not write the quit-intent sentinel %s", qPrintable(path));
    return false;
  }
  sentinel.write(QByteArray::number(QCoreApplication::applicationPid()) + '\n');
  return true;
}

bool isOSXAccessibilityGranted(bool promptUser)
{
  NSDictionary *options = @{(__bridge id)kAXTrustedCheckOptionPrompt : promptUser ? @YES : @NO};
  return AXIsProcessTrustedWithOptions((__bridge CFDictionaryRef)options);
}

void openOSXAccessibilitySettings()
{
  NSURL *url =
      [NSURL URLWithString:@"x-apple.systempreferences:com.apple.preference.security?Privacy_Accessibility"];
  [[NSWorkspace sharedWorkspace] openURL:url];
}
