# Copyright 2012 The Chromium Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

"""Seeds a number of variables defined in chromium_config.py.

The recommended way is to fork this file and use a custom DEPS forked from
config/XXX/DEPS with the right configuration data."""

import socket


class classproperty(object):
  """A decorator that allows is_production_host to only to be defined once."""
  def __init__(self, getter):
    self.getter = getter
  def __get__(self, instance, owner):
    return self.getter(owner)


class Master(object):
  # Repository URLs used by the SVNPoller and 'gclient config'.
  server_url = 'http://src.chromium.org'
  repo_root = '/svn'
  git_server_url = 'https://chromium.googlesource.com'

  # External repos.
  googlecode_url = 'http://%s.googlecode.com/svn'
  sourceforge_url = 'https://svn.code.sf.net/p/%(repo)s/code'
  googlecode_revlinktmpl = 'https://code.google.com/p/%s/source/browse?r=%s'

  # Directly fetches from anonymous Blink svn server.
  webkit_root_url = 'http://src.chromium.org/blink'
  nacl_trunk_url = 'http://src.chromium.org/native_client/trunk'

  llvm_url = 'http://llvm.org/svn/llvm-project'

  # Perf Dashboard upload URL.
  dashboard_upload_url = 'https://chromeperf.appspot.com'

  # Actually for Chromium OS slaves.
  chromeos_url = git_server_url + '/chromiumos.git'

  # Default domain for emails to come from and
  # domains to which emails can be sent.
  master_domain = 'example.com'
  permitted_domains = ('example.com',)

  # Your smtp server to enable mail notifications.
  smtp = 'smtp'

  # By default, bot_password will be filled in by config.GetBotPassword().
  bot_password = None

  # Fake urls to make various factories happy.
  swarm_server_internal_url = 'http://fake.swarm.url.server.com'
  swarm_server_dev_internal_url = 'http://fake.swarm.dev.url.server.com'
  swarm_hashtable_server_internal = 'http://fake.swarm.hashtable.server.com'
  swarm_hashtable_server_dev_internal = 'http://fake.swarm.hashtable.server.com'
  trunk_internal_url = None
  trunk_internal_url_src = None
  slave_internal_url = None
  git_internal_server_url = None
  syzygy_internal_url = None
  webrtc_internal_url = None
  webrtc_limited_url = None
  v8_internal_url = None


  class Base(object):
    """Master base template.
    Contains stubs for variables that all masters must define."""
    # Master address. You should probably copy this file in another svn repo
    # so you can override this value on both the slaves and the master.
    master_host = 'localhost'
    # Only report that we are running on a master if the master_host (even when
    # master_host is overridden by a subclass) is the same as the current host.
    @classproperty
    def is_production_host(cls):
      return socket.getfqdn() == cls.master_host
    # 'from:' field for emails sent from the server.
    from_address = 'nobody@example.com'
    # Additional email addresses to send gatekeeper (automatic tree closage)
    # notifications. Unnecessary for experimental masters and try servers.
    tree_closing_notification_recipients = []
    # For the following values, they are used only if non-0. Do not set them
    # here, set them in the actual master configuration class:
    # Used for the waterfall URL and the waterfall's WebStatus object.
    master_port = 0
    # Which port slaves use to connect to the master.
    slave_port = 0
    # The alternate read-only page. Optional.
    master_port_alt = 0

  ## Per-master configs.

  class Master1(Base):
    """Chromium master."""
    master_host = 'master1.golo.chromium.org'
    from_address = 'buildbot@chromium.org'
    tree_closing_notification_recipients = [
        'chromium-build-failure@chromium-gatekeeper-sentry.appspotmail.com']
    base_app_url = 'https://chromium-status.appspot.com'
    tree_status_url = base_app_url + '/status'
    store_revisions_url = base_app_url + '/revisions'
    last_good_url = base_app_url + '/lkgr'
    last_good_blink_url = 'http://blink-status.appspot.com/lkgr'

  class Master2(Base):
    """Chromeos master."""
    master_host = 'master2.golo.chromium.org'
    tree_closing_notification_recipients = [
        'chromeos-build-failures@google.com']
    from_address = 'buildbot@chromium.org'

  class Master3(Base):
    """Client master."""
    master_host = 'master3.golo.chromium.org'
    tree_closing_notification_recipients = []
    from_address = 'buildbot@chromium.org'

  class Master4(Base):
    """Try server master."""
    master_host = 'master4.golo.chromium.org'
    tree_closing_notification_recipients = []
    from_address = 'tryserver@chromium.org'
    code_review_site = 'https://codereview.chromium.org'

  ## Native Client related

  class NaClBase(Master3):
    """Base class for Native Client masters."""
    tree_closing_notification_recipients = ['bradnelson@chromium.org']
    base_app_url = 'https://nativeclient-status.appspot.com'
    tree_status_url = base_app_url + '/status'
    store_revisions_url = base_app_url + '/revisions'
    last_good_url = base_app_url + '/lkgr'
    perf_base_url = 'http://build.chromium.org/f/client/perf'

  ## ChromiumOS related

  class ChromiumOSBase(Master2):
    """Base class for ChromiumOS masters"""
    base_app_url = 'https://chromiumos-status.appspot.com'
    tree_status_url = base_app_url + '/status'
    store_revisions_url = base_app_url + '/revisions'
    last_good_url = base_app_url + '/lkgr'

  class ChromiumOSTryServer(Master2):
    project_name = 'ChromiumOS Try Server'
    master_port = 9051
    slave_port = 9153
    master_port_alt = 9063
    repo_url_ext = 'https://git.chromium.org/chromiumos/tryjobs.git'
    repo_url_int = None
    # The reply-to address to set for emails sent from the server.
    reply_to = 'nobody@example.com'
