// Copyright 2015 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#import "ios/chrome/browser/open_from_clipboard/create_clipboard_recent_content.h"

#import "components/open_from_clipboard/clipboard_recent_content_ios.h"
#import "ios/chrome/browser/shared/public/features/features.h"
#import "ios/chrome/browser/url/chrome_url_constants.h"
#import "ios/chrome/common/app_group/app_group_constants.h"
#import "ios/components/webui/web_ui_url_constants.h"

#if !defined(__has_feature) || !__has_feature(objc_arc)
#error "This file requires ARC support."
#endif

std::unique_ptr<ClipboardRecentContent> CreateClipboardRecentContentIOS() {
  return std::make_unique<ClipboardRecentContentIOS>(
      kChromeUIScheme, app_group::GetGroupUserDefaults(),
      base::FeatureList::IsEnabled(kOnlyAccessClipboardAsync));
}
