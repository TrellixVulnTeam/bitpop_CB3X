# Copyright (c) 2012 The Chromium Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

from master import gatekeeper
from master import master_utils

# This is the list of the builder categories and the corresponding critical
# steps. If one critical step fails, gatekeeper will close the tree
# automatically.
# Note: don't include 'update scripts' since we can't do much about it when
# it's failing and the tree is still technically fine.
categories_steps = {
  '': ['update', 'runhooks'],
  'testers': [
    'app_list_unittests',
    'ash_unittests',
    'aura_unittests',
    'base_unittests',
    'browser_tests',
    'cacheinvalidation_unittests',
    'cast_unittests',
    'cc_unittests',
    'chrome_elf_unittests',
    'chromedriver_unittests',
    'components_unittests',
    'compositor_unittests',
    'content_browsertests',
    'content_unittests',
    'courgette_unittests',
    'crypto_unittests',
    'device_unittests',
    'events_unittests',
    'gcm_unit_tests',
    'gfx_unittests',
    'google_apis_unittests',
    'installer_util_unittests',
    'interactive_ui_tests',
    'ipc_tests',
    'jingle_unittests',
    'media_unittests',
    'mini_installer_test',
    'nacl_integration',
    'net_unittests',
    'ppapi_unittests',
    'printing_unittests',
    'remoting_unittests',
    'sandbox_linux_unittests',
    'sbox_integration_tests',
    'sbox_unittests',
    'sbox_validation_tests',
    'sizes',
    'sql_unittests',
    'start_crash_handler',
    'sync_integration_tests',
    'sync_unit_tests',
    'ui_unittests',
    'unit_tests',
    'url_unittests',
    'views_unittests',
    'webkit_compositor_bindings_unittests',
    #'webkit_tests',
   ],
  'windows': ['svnkill', 'taskkill'],
  'compile': ['check_deps2git', 'check_deps', 'compile', 'archive_build'],
  # Annotator scripts are triggered as a 'slave_steps' step.
  # The gatekeeper currently does not recognize failure in a
  # @@@BUILD_STEP@@@, so we must match on the buildbot-defined step.
  'android': ['slave_steps'],
}

exclusions = {
}

forgiving_steps = ['update_scripts', 'update', 'svnkill', 'taskkill',
                   'archive_build', 'start_crash_handler', 'gclient_revert']

def Update(config, active_master, c):
  c['status'].append(gatekeeper.GateKeeper(
      fromaddr=active_master.from_address,
      categories_steps=categories_steps,
      exclusions=exclusions,
      relayhost=config.Master.smtp,
      subject='buildbot %(result)s in %(projectName)s on %(builder)s, '
              'revision %(revision)s',
      extraRecipients=active_master.tree_closing_notification_recipients,
      lookup=master_utils.FilterDomain(),
      forgiving_steps=forgiving_steps,
      tree_status_url=active_master.tree_status_url,
      public_html='../master.chromium/public_html',
      use_getname=True))

  # Notify nacl-broke@google.com when nacl_integration fails.
  c['status'].append(gatekeeper.GateKeeper(
      fromaddr=active_master.from_address,
      categories_steps=['nacl_integration'],
      exclusions=exclusions,
      relayhost=config.Master.smtp,
      subject='buildbot %(result)s in %(projectName)s on %(builder)s, '
              'revision %(revision)s',
      sheriffs=None,
      extraRecipients=['nacl-broke@google.com'],
      lookup=master_utils.FilterDomain(),
      forgiving_steps=forgiving_steps,
      tree_status_url=None,
      public_html='../master.chromium/public_html',
      use_getname=True))
