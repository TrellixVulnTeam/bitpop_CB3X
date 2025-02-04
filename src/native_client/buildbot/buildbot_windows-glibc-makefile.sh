#!/bin/bash
# Copyright (c) 2012 The Native Client Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

# Script assumed to be run in native_client/
cd "$(cygpath "${PWD}")"
if [[ ${PWD} != */native_client ]]; then
  echo "ERROR: must be run in native_client!"
  exit 1
fi

set -x
set -e
set -u

# Transitionally, even though our new toolchain location is under
# toolchain/win_x86_nacl_x86/nacl_x86_glibc we have to keep the old format
# inside of the tar (toolchain/win_x86) so that the untar toolchain script
# is backwards compatible and can untar old tars. Eventually this will be
# unnecessary with the new package_version scheme since how to untar the
# tar file will be embedded inside of the package file so they can differ
# between revisions.
export TOOLCHAINLOC=toolchain
export TOOLCHAINNAME=win_x86

# This is where we want the toolchain when moving to native_client/toolchain.
OUT_TOOLCHAINLOC=toolchain/win_x86_nacl_x86
OUT_TOOLCHAINNAME=nacl_x86_glibc

TOOL_TOOLCHAIN="$TOOLCHAINLOC/$TOOLCHAINNAME"
OUT_TOOLCHAIN="${OUT_TOOLCHAINLOC}/${OUT_TOOLCHAINNAME}"

export INST_GLIBC_PROGRAM="$PWD/tools/glibc_download.sh"
# Workaround for broken autoconf mmap test (WOW64 limitation)
# More info here: http://cygwin.com/ml/cygwin/2011-03/msg00596.html
export ac_cv_func_mmap_fixed_mapped=yes

echo @@@BUILD_STEP clobber_toolchain@@@
rm -rf scons-out tools/BUILD/* tools/out tools/toolchain \
  tools/glibc tools/glibc.tar tools/toolchain.t* "${OUT_TOOLCHAIN}" .tmp ||
  echo already_clean
mkdir -p "tools/${TOOL_TOOLCHAIN}"
ln -sfn "$PWD"/cygwin/tmp "tools/${TOOL_TOOLCHAIN}"

echo @@@BUILD_STEP clean_sources@@@
tools/update_all_repos_to_latest.sh

# glibc_download.sh can return three return codes:
#  0 - glibc is successfully downloaded and installed
#  1 - glibc is not downloaded but another run may help
#  2+ - glibc is not downloaded and can not be downloaded later
#
# If the error result is 2 or more we are stopping the build
echo @@@BUILD_STEP check_glibc_revision_sanity@@@
echo "Try to download glibc revision $(tools/glibc_revision.sh)"
if tools/glibc_download.sh "tools/${TOOL_TOOLCHAIN}" 1; then
  INST_GLIBC_PROGRAM=true
elif (($?>1)); then
  echo @@@STEP_FAILURE@@@
  exit 100
fi

if [[ "${BUILDBOT_SLAVE_TYPE:-Trybot}" == "Trybot" ]]; then
echo @@@BUILD_STEP setup source@@@
(cd tools; ./buildbot_patch-toolchain-tries.sh)
fi

echo @@@BUILD_STEP compile_toolchain@@@
(
  cd tools
  make -j8 buildbot-build-with-glibc
  rm "${TOOL_TOOLCHAIN}/tmp"
)

if [[ "${BUILDBOT_SLAVE_TYPE:-Trybot}" != "Trybot" ]]; then
  GSD_BUCKET=nativeclient-archive2
  UPLOAD_REV=${BUILDBOT_GOT_REVISION}
  UPLOAD_LOC=x86_toolchain/r${UPLOAD_REV}
else
  GSD_BUCKET=nativeclient-trybot/packages
  UPLOAD_REV=${BUILDBOT_BUILDERNAME}/${BUILDBOT_BUILDNUMBER}
  UPLOAD_LOC=x86_toolchain/${UPLOAD_REV}
fi

(
  cd tools
  echo @@@BUILD_STEP canonicalize timestamps@@@
  ./canonicalize_timestamps.sh "${TOOL_TOOLCHAIN}"
  echo @@@BUILD_STEP tar_toolchain@@@
  tar Scf toolchain.tar "${TOOL_TOOLCHAIN}"
  xz -k -9 toolchain.tar
  bzip2 -k -9 toolchain.tar
  gzip -n -9 toolchain.tar
  for i in gz bz2 xz ; do
    chmod a+x toolchain.tar.$i
    echo "$(SHA1=$(sha1sum -b toolchain.tar.$i) ; echo ${SHA1:0:40})" \
      > toolchain.tar.$i.sha1hash
  done
)

echo @@@BUILD_STEP archive_build@@@
for suffix in gz gz.sha1hash bz2 bz2.sha1hash xz xz.sha1hash ; do
  ${NATIVE_PYTHON} ${GSUTIL} cp -a public-read \
    tools/toolchain.tar.$suffix \
    gs://${GSD_BUCKET}/${UPLOAD_LOC}/toolchain_win_x86.tar.$suffix
done
echo @@@STEP_LINK@download@http://gsdview.appspot.com/${GSD_BUCKET}/${UPLOAD_LOC}/@@@

echo @@@BUILD_STEP archive_extract_package@@@
${NATIVE_PYTHON} build/package_version/package_version.py \
  archive --archive-package=nacl_x86_glibc --extract \
  --extra-archive gdb_i686_w64_mingw32.tgz \
  tools/toolchain.tar.bz2,toolchain/win_x86@https://storage.googleapis.com/${GSD_BUCKET}/${UPLOAD_LOC}/toolchain_win_x86.tar.bz2 \

echo @@@BUILD_STEP upload_package_info@@@
${NATIVE_PYTHON} build/package_version/package_version.py \
  --cloud-bucket=${GSD_BUCKET} --annotate \
  upload --skip-missing \
  --upload-package=nacl_x86_glibc --revision=${UPLOAD_REV}

# sync_backports is obsolete and should probably be removed.
# if [[ "${BUILD_COMPATIBLE_TOOLCHAINS:-yes}" != "no" ]]; then
#   echo @@@BUILD_STEP sync backports@@@
#   rm -rf tools/BACKPORTS/ppapi*
#   tools/BACKPORTS/build_backports.sh VERSIONS win glibc
# fi
