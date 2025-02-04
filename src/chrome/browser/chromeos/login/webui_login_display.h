// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_CHROMEOS_LOGIN_WEBUI_LOGIN_DISPLAY_H_
#define CHROME_BROWSER_CHROMEOS_LOGIN_WEBUI_LOGIN_DISPLAY_H_

#include <string>
#include <vector>

#include "base/compiler_specific.h"
#include "base/timer/timer.h"
#include "chrome/browser/chromeos/login/login_display.h"
#include "chrome/browser/chromeos/login/user.h"
#include "chrome/browser/ui/webui/chromeos/login/native_window_delegate.h"
#include "chrome/browser/ui/webui/chromeos/login/signin_screen_handler.h"
#include "ui/views/widget/widget.h"
#include "ui/wm/core/user_activity_observer.h"

namespace chromeos {
// WebUI-based login UI implementation.
class WebUILoginDisplay : public LoginDisplay,
                          public NativeWindowDelegate,
                          public SigninScreenHandlerDelegate,
                          public wm::UserActivityObserver {
 public:
  explicit WebUILoginDisplay(LoginDisplay::Delegate* delegate);
  virtual ~WebUILoginDisplay();

  // LoginDisplay implementation:
  virtual void ClearAndEnablePassword() OVERRIDE;
  virtual void Init(const UserList& users,
                    bool show_guest,
                    bool show_users,
                    bool show_new_user) OVERRIDE;
  virtual void OnPreferencesChanged() OVERRIDE;
  virtual void OnBeforeUserRemoved(const std::string& username) OVERRIDE;
  virtual void OnUserImageChanged(const User& user) OVERRIDE;
  virtual void OnUserRemoved(const std::string& username) OVERRIDE;
  virtual void OnFadeOut() OVERRIDE;
  virtual void OnLoginSuccess(const std::string& username) OVERRIDE;
  virtual void SetUIEnabled(bool is_enabled) OVERRIDE;
  virtual void SelectPod(int index) OVERRIDE;
  virtual void ShowBannerMessage(const std::string& message) OVERRIDE;
  virtual void ShowUserPodButton(const std::string& username,
                                 const std::string& iconURL,
                                 const base::Closure& click_callback) OVERRIDE;
  virtual void HideUserPodButton(const std::string& username) OVERRIDE;
  virtual void SetAuthType(const std::string& username,
                           AuthType auth_type,
                           const std::string& initial_value) OVERRIDE;
  virtual AuthType GetAuthType(const std::string& username) const OVERRIDE;
  virtual void ShowError(int error_msg_id,
                         int login_attempts,
                         HelpAppLauncher::HelpTopic help_topic_id) OVERRIDE;
  virtual void ShowErrorScreen(LoginDisplay::SigninError error_id) OVERRIDE;
  virtual void ShowGaiaPasswordChanged(const std::string& username) OVERRIDE;
  virtual void ShowPasswordChangedDialog(bool show_password_error) OVERRIDE;
  virtual void ShowSigninUI(const std::string& email) OVERRIDE;

  // NativeWindowDelegate implementation:
  virtual gfx::NativeWindow GetNativeWindow() const OVERRIDE;

  // SigninScreenHandlerDelegate implementation:
  virtual void CancelPasswordChangedFlow() OVERRIDE;
  virtual void CancelUserAdding() OVERRIDE;
  virtual void CreateAccount() OVERRIDE;
  virtual void CompleteLogin(const UserContext& user_context) OVERRIDE;
  virtual void Login(const UserContext& user_context) OVERRIDE;
  virtual void LoginAsRetailModeUser() OVERRIDE;
  virtual void LoginAsGuest() OVERRIDE;
  virtual void MigrateUserData(const std::string& old_password) OVERRIDE;
  virtual void LoginAsPublicAccount(const std::string& username) OVERRIDE;
  virtual void LoadWallpaper(const std::string& username) OVERRIDE;
  virtual void LoadSigninWallpaper() OVERRIDE;
  virtual void OnSigninScreenReady() OVERRIDE;
  virtual void RemoveUser(const std::string& username) OVERRIDE;
  virtual void ResyncUserData() OVERRIDE;
  virtual void ShowEnterpriseEnrollmentScreen() OVERRIDE;
  virtual void ShowKioskEnableScreen() OVERRIDE;
  virtual void ShowKioskAutolaunchScreen() OVERRIDE;
  virtual void ShowWrongHWIDScreen() OVERRIDE;
  virtual void SetWebUIHandler(
      LoginDisplayWebUIHandler* webui_handler) OVERRIDE;
  virtual void ShowSigninScreenForCreds(const std::string& username,
                                        const std::string& password);
  virtual const UserList& GetUsers() const OVERRIDE;
  virtual bool IsShowGuest() const OVERRIDE;
  virtual bool IsShowUsers() const OVERRIDE;
  virtual bool IsShowNewUser() const OVERRIDE;
  virtual bool IsSigninInProgress() const OVERRIDE;
  virtual bool IsUserSigninCompleted() const OVERRIDE;
  virtual void SetDisplayEmail(const std::string& email) OVERRIDE;
  virtual void Signout() OVERRIDE;
  virtual void LoginAsKioskApp(const std::string& app_id,
                               bool diagnostic_mode) OVERRIDE;

  // wm::UserActivityDetector implementation:
  virtual void OnUserActivity(const ui::Event* event) OVERRIDE;

 private:
  void StartPasswordClearTimer();
  void OnPasswordClearTimerExpired();

  // Set of Users that are visible.
  UserList users_;

  // Whether to show guest login.
  bool show_guest_;

  // Weather to show the user pods or a GAIA sign in.
  // Public sessions are always shown.
  bool show_users_;

  // Whether to show add new user.
  bool show_new_user_;

  // Timer for measuring idle state duration before password clear.
  base::OneShotTimer<WebUILoginDisplay> password_clear_timer_;

  // Reference to the WebUI handling layer for the login screen
  LoginDisplayWebUIHandler* webui_handler_;

  DISALLOW_COPY_AND_ASSIGN(WebUILoginDisplay);
};

}  // namespace chromeos

#endif  // CHROME_BROWSER_CHROMEOS_LOGIN_WEBUI_LOGIN_DISPLAY_H_
