#!/usr/bin/python
# Copyright (c) 2012 The Native Client Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

"""Memoize the data produced by slow operations into Google storage.

Caches computations described in terms of command lines and inputs directories
or files, which yield a set of output file.
"""

import hashlib
import logging
import os
import platform
import shutil
import subprocess
import sys

sys.path.append(os.path.join(os.path.dirname(__file__), '..'))
import pynacl.directory_storage
import pynacl.file_tools
import pynacl.gsd_storage
import pynacl.hashing_tools
import pynacl.working_directory

import command
import substituter


class HumanReadableSignature(object):
  """Accumator of signature information in human readable form.

  A replacement for hashlib that collects the inputs for later display.
  """
  def __init__(self):
    self._items = []

  def update(self, data):
    """Add an item to the signature."""
    # Drop paranoid nulls for human readable output.
    data = data.replace('\0', '')
    self._items.append(data)

  def hexdigest(self):
    """Fake version of hexdigest that returns the inputs."""
    return ('*' * 30 + ' PACKAGE SIGNATURE ' + '*' * 30 + '\n' +
            '\n'.join(self._items) + '\n' +
            '=' * 70 + '\n')


class Once(object):
  """Class to memoize slow operations."""

  def __init__(self, storage, use_cached_results=True, cache_results=True,
               print_url=None, system_summary=None):
    """Constructor.

    Args:
      storage: An storage layer to read/write from (GSDStorage).
      use_cached_results: Flag indicating that cached computation results
                          should be used when possible.
      cache_results: Flag that indicates if successful computations should be
                     written to the cache.
      print_url: Function that accepts an URL for printing the build result,
                 or None.
    """
    self._storage = storage
    self._directory_storage = pynacl.directory_storage.DirectoryStorageAdapter(
        storage
    )
    self._use_cached_results = use_cached_results
    self._cache_results = cache_results
    self._cached_dir_items = {}
    self._print_url = print_url
    self._system_summary = system_summary
    self._path_hash_cache = {}

  def KeyForOutput(self, package, output_hash):
    """Compute the key to store a give output in the data-store.

    Args:
      package: Package name.
      output_hash: Stable hash of the package output.
    Returns:
      Key that this instance of the package output should be stored/retrieved.
    """
    return 'object/%s_%s.tgz' % (package, output_hash)

  def KeyForBuildSignature(self, build_signature):
    """Compute the key to store a computation result in the data-store.

    Args:
      build_signature: Stable hash of the computation.
    Returns:
      Key that this instance of the computation result should be
      stored/retrieved.
    """
    return 'computed/%s.txt' % build_signature

  def WriteOutputFromHash(self, package, out_hash, output):
    """Write output from the cache.

    Args:
      package: Package name (for tgz name).
      out_hash: Hash of desired output.
      output: Output path.
    Returns:
      URL from which output was obtained if successful, or None if not.
    """
    key = self.KeyForOutput(package, out_hash)
    dir_item = self._directory_storage.GetDirectory(key, output)
    if not dir_item:
      logging.debug('Failed to retrieve %s' % key)
      return None
    if pynacl.hashing_tools.StableHashPath(output) != out_hash:
      logging.warning('Object does not match expected hash, '
                      'has hashing method changed?')
      return None
    return dir_item

  def _ProcessCachedDir(self, package, dir_item):
    """Processes cached directory storage items.

    Args:
      package: Package name for the cached directory item.
      dir_item: DirectoryStorageItem returned from directory_storage.
    """
    # Store the cached URL as a tuple for book keeping.
    self._cached_dir_items[package] = dir_item

    # If a print URL function has been specified, print the URL now.
    if self._print_url is not None:
      self._print_url(dir_item.url)

  def WriteResultToCache(self, package, build_signature, output):
    """Cache a computed result by key.

    Also prints URLs when appropriate.
    Args:
      package: Package name (for tgz name).
      build_signature: The input hash of the computation.
      output: A path containing the output of the computation.
    """
    if not self._cache_results:
      return
    out_hash = pynacl.hashing_tools.StableHashPath(output)
    try:
      output_key = self.KeyForOutput(package, out_hash)
      # Try to get an existing copy in a temporary directory.
      wd = pynacl.working_directory.TemporaryWorkingDirectory()
      with wd as work_dir:
        temp_output = os.path.join(work_dir, 'out')
        dir_item = self._directory_storage.GetDirectory(output_key, temp_output)
        if dir_item is None:
          # Isn't present. Cache the computed result instead.
          dir_item = self._directory_storage.PutDirectory(output, output_key)
          logging.info('Computed fresh result and cached it.')
        else:
          # Cached version is present. Replace the current output with that.
          if self._use_cached_results:
            pynacl.file_tools.RemoveDirectoryIfPresent(output)
            shutil.move(temp_output, output)
            logging.info(
                'Recomputed result matches cached value, '
                'using cached value instead.')
      # Upload an entry mapping from computation input to output hash.
      self._storage.PutData(
          out_hash, self.KeyForBuildSignature(build_signature))
      self._ProcessCachedDir(package, dir_item)
    except pynacl.gsd_storage.GSDStorageError:
      logging.info('Failed to cache result.')
      raise

  def ReadMemoizedResultFromCache(self, package, build_signature, output):
    """Read a cached result (if it exists) from the cache.

    Also prints URLs when appropriate.
    Args:
      package: Package name (for tgz name).
      build_signature: Build signature of the computation.
      output: Output path.
    Returns:
      Boolean indicating successful retrieval.
    """
    # Check if its in the cache.
    if self._use_cached_results:
      out_hash = self._storage.GetData(
          self.KeyForBuildSignature(build_signature))
      if out_hash is not None:
        dir_item = self.WriteOutputFromHash(package, out_hash, output)
        if dir_item is not None:
          logging.info('Retrieved cached result.')
          self._ProcessCachedDir(package, dir_item)
          return True
    return False

  def GetCachedDirItems(self):
    """Returns the complete list of all cached directory items for this run."""
    return self._cached_dir_items.values()

  def GetCachedDirItemForPackage(self, package):
    """Returns cached directory item for package or None if not processed."""
    return self._cached_dir_items.get(package, None)

  def Run(self, package, inputs, output, commands,
          working_dir=None, memoize=True, signature_file=None, subdir=None):
    """Run an operation once, possibly hitting cache.

    Args:
      package: Name of the computation/module.
      inputs: A dict of names mapped to files that are inputs.
      output: An output directory.
      commands: A list of command.Command objects to run.
      working_dir: Working directory to use, or None for a temp dir.
      memoize: Boolean indicating the the result should be memoized.
      signature_file: File to write human readable build signatures to or None.
      subdir: If not None, use this directory instead of the output dir as the
              substituter's output path. Must be a subdirectory of output.
    """
    if working_dir is None:
      wdm = pynacl.working_directory.TemporaryWorkingDirectory()
    else:
      wdm = pynacl.working_directory.FixedWorkingDirectory(working_dir)

    pynacl.file_tools.MakeDirectoryIfAbsent(output)

    nonpath_subst = { 'package': package }

    with wdm as work_dir:
      # Compute the build signature with modified inputs.
      build_signature = self.BuildSignature(
          package, inputs=inputs, commands=commands)
      # Optionally write human readable version of signature.
      if signature_file:
        signature_file.write(self.BuildSignature(
            package, inputs=inputs, commands=commands,
            hasher=HumanReadableSignature()))
        signature_file.flush()

      # We're done if it's in the cache.
      if (memoize and
          self.ReadMemoizedResultFromCache(package, build_signature, output)):
        return

      if subdir:
        assert subdir.startswith(output)

      for command in commands:
        paths = inputs.copy()
        paths['output'] = subdir if subdir else output
        nonpath_subst['build_signature'] = build_signature
        subst = substituter.Substituter(work_dir, paths, nonpath_subst)
        command.Invoke(subst)

    if memoize:
      self.WriteResultToCache(package, build_signature, output)

  def SystemSummary(self):
    """Gather a string describing intrinsic properties of the current machine.

    Ideally this would capture anything relevant about the current machine that
    would cause build output to vary (other than build recipe + inputs).
    """
    if self._system_summary is None:
      # Note there is no attempt to canonicalize these values.  If two
      # machines that would in fact produce identical builds differ in
      # these values, it just means that a superfluous build will be
      # done once to get the mapping from new input hash to preexisting
      # output hash into the cache.
      assert len(sys.platform) != 0, len(platform.machine()) != 0
      # Use environment from command so we can access MinGW on windows.
      env = command.PlatformEnvironment([])
      gcc = pynacl.file_tools.Which('gcc', paths=env['PATH'].split(os.pathsep))
      p = subprocess.Popen(
          [gcc, '-v'], stdout=subprocess.PIPE, stderr=subprocess.PIPE, env=env)
      _, gcc_version = p.communicate()
      assert p.returncode == 0
      items = [
          ('platform', sys.platform),
          ('machine', platform.machine()),
          ('gcc-v', gcc_version),
          ]
      self._system_summary = str(items)
    return self._system_summary

  def BuildSignature(self, package, inputs, commands, hasher=None):
    """Compute a total checksum for a computation.

    The computed hash includes system properties, inputs, and the commands run.
    Args:
      package: The name of the package computed.
      inputs: A dict of names -> files/directories to be included in the
              inputs set.
      commands: A list of command.Command objects describing the commands run
                for this computation.
      hasher: Optional hasher to use.
    Returns:
      A hex formatted sha1 to use as a computation key or a human readable
      signature.
    """
    if hasher is None:
      h = hashlib.sha1()
    else:
      h = hasher

    h.update('package:' + package)
    h.update('summary:' + self.SystemSummary())
    for command in commands:
      h.update('command:')
      h.update(str(command))
    for key in sorted(inputs.keys()):
      h.update('item_name:' + key + '\x00')
      if inputs[key] in self._path_hash_cache:
        path_hash = self._path_hash_cache[inputs[key]]
      else:
        path_hash = 'item:' + pynacl.hashing_tools.StableHashPath(inputs[key])
        self._path_hash_cache[inputs[key]] = path_hash
      h.update(path_hash)
    return h.hexdigest()
