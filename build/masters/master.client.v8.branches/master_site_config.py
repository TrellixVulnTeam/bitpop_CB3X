# Copyright 2013 The Chromium Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

"""ActiveMaster definition."""

from config_bootstrap import Master

class V8Branches(Master.Master3):
  base_app_url = 'https://v8-status.appspot.com'
  tree_status_url = base_app_url + '/status'
  store_revisions_url = base_app_url + '/revisions'
  last_good_url = base_app_url + '/lkgr'
  project_name = 'V8 Branches'
  master_port = 8046
  slave_port = 8146
  master_port_alt = 8246
  project_url = 'http://v8.googlecode.com'
