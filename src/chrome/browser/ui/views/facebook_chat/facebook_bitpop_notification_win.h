// BitPop browser with features like Facebook chat and uncensored browsing. 
// Copyright (C) 2014 BitPop AS
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.

#ifndef CHROME_BROWSER_UI_VIEWS_FACEBOOK_CHAT_FACEBOOK_BITPOP_NOTIFICATION_WIN_H_
#define CHROME_BROWSER_UI_VIEWS_FACEBOOK_CHAT_FACEBOOK_BITPOP_NOTIFICATION_WIN_H_

#pragma once

#include "base/compiler_specific.h"
#include "base/win/win_util.h"
#include "chrome/browser/facebook_chat/facebook_bitpop_notification.h"

class Profile;

class FacebookBitpopNotificationWin : public FacebookBitpopNotification {
public:
  FacebookBitpopNotificationWin(Profile* profile);
  virtual ~FacebookBitpopNotificationWin();

  virtual void ClearNotification() OVERRIDE;
  virtual void NotifyUnreadMessagesWithLastUser(int num_unread,
                                                const std::string& user_id) OVERRIDE;

  virtual void Shutdown();
protected:
  Profile *profile_;
  HWND notified_hwnd_;
};

#endif  // CHROME_BROWSER_UI_VIEWS_FACEBOOK_CHAT_FACEBOOK_BITPOP_NOTIFICATION_WIN_H_