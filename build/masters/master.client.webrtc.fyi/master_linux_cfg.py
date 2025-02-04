# Copyright 2013 The Chromium Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

from master import master_config
from master.factory import webrtc_factory

defaults = {}

def linux():
  return webrtc_factory.WebRTCFactory('src/out', 'linux2')

helper = master_config.Helper(defaults)
B = helper.Builder
F = helper.Factory
S = helper.Scheduler

scheduler = 'webrtc_linux_scheduler'
S(scheduler, branch='trunk', treeStableTimer=0)

tests = [
    'audio_decoder_unittests',
    'common_audio_unittests',
    'common_video_unittests',
    'libjingle_media_unittest',
    'libjingle_p2p_unittest',
    'libjingle_peerconnection_unittest',
    'libjingle_sound_unittest',
    'libjingle_unittest',
    'modules_tests',
    'modules_unittests',
    'system_wrappers_unittests',
    'test_support_unittests',
    'tools_unittests',
    'video_engine_core_unittests',
    'video_engine_tests',
    'voice_engine_unittests',
]

defaults['category'] = 'linux'

B('Linux TsanRV', 'linux_tsan_rv_factory', scheduler=scheduler)
F('linux_tsan_rv_factory', linux().WebRTCFactory(
    target='Release',
    tests=['tsan_rv_' + test for test in tests],
    options=['--compiler=goma'],
    factory_properties={
        'needs_valgrind': True,
        'gclient_env': {'GYP_DEFINES': 'build_for_tool=tsan'}}))

B('Linux LSan (and ASan)', 'linux_lsan_factory', scheduler=scheduler)
F('linux_lsan_factory', linux().WebRTCFactory(
    target='Release',
    tests=tests,
    options=['--compiler=goma-clang'],
    factory_properties={
        'lsan': True,
        'asan': True,
        'gclient_env': {
            'GYP_DEFINES': ('asan=1 '
                            'lsan=1 '
                            'use_allocator=none '
                            'release_extra_cflags=-g ')}}))


def Update(c):
  helper.Update(c)
