# Copyright 2014 The Chromium Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

import argparse
import multiprocessing
import sys

from .cover import CoverageContext

from . import handle_list, handle_debug, handle_train, handle_test

from .pipeline import result_loop


HANDLERS = {
  'list': handle_list.ListHandler,
  'debug': handle_debug.DebugHandler,
  'train': handle_train.TrainHandler,
  'test': handle_test.TestHandler,
}


def _parse_args(args):
  args = args or sys.argv[1:]

  # Set the default mode if not specified and not passing --help
  search_names = set(HANDLERS.keys() + ['-h', '--help'])
  if not any(arg in search_names for arg in args):
    args.insert(0, 'test')

  parser = argparse.ArgumentParser()
  subparsers = parser.add_subparsers(
      title='Mode (default "test")', dest='mode',
      help='See `[mode] --help` for more options.')

  for k, h in HANDLERS.iteritems():
    doc = h.__doc__
    if doc:
      doc = doc[0].lower() + doc[1:]
    sp = subparsers.add_parser(k, help=doc)
    h.add_options(sp)

    mg = sp.add_mutually_exclusive_group()
    mg.add_argument(
        '--quiet', action='store_true',
        help='be quiet (only print failures)')
    mg.add_argument(
        '--verbose', action='store_true', help='be verbose')

    if not h.SKIP_RUNLOOP:
      sp.add_argument(
          '--jobs', metavar='N', type=int,
          default=multiprocessing.cpu_count(),
          help='run N jobs in parallel (default %(default)s)')

    sp.add_argument(
        '--test_list', metavar='FILE',
        help='take the list of test globs from the FILE (use "-" for stdin)')

    sp.add_argument(
        'test_glob', nargs='*', help=(
            'glob to filter the tests acted on. If the glob begins with "-" '
            'then it acts as a negation glob and anything which matches it '
            'will be skipped. If a glob doesn\'t have "*" in it, "*" will be '
            'implicitly appended to the end'))

  opts = parser.parse_args(args)

  if not hasattr(opts, 'jobs'):
    opts.jobs = 0
  elif opts.jobs < 1:
    parser.error('--jobs was less than 1')

  if opts.test_list:
    fh = sys.stdin if opts.test_list == '-' else open(opts.test_list, 'rb')
    with fh as tl:
      opts.test_glob += [l.strip() for l in tl.readlines()]

  opts.handler = HANDLERS[opts.mode]

  del opts.test_list
  del opts.mode

  return opts


def main(name, test_gen, coverage_includes=None, coverage_omits=None,
         args=None):
  """Entry point for tests using expect_tests.

  Example:
    import expect_tests

    def happy_fn(val):
      # Usually you would return data which is the result of some deterministic
      # computation.
      return expect_tests.Result({'neet': '%s string value' % val})

    def Gen():
      yield expect_tests.Test('happy', happy_fn, args=('happy',))

    if __name__ == '__main__':
      expect_tests.main('happy_test_suite', Gen)

  @param name: Name of the test suite.
  @param test_gen: A Generator which yields Test objects.
  @param coverage_includes: A list of path globs to include under coverage.
  @param coverage_omits: A list of path globs to exclude under coverage.
  @param args: Commandline args (starting at argv[1])
  """
  try:
    opts = _parse_args(args)

    cover_ctx = CoverageContext(name, coverage_includes, coverage_omits,
                                not opts.handler.SKIP_RUNLOOP)

    error, killed = result_loop(test_gen, cover_ctx.create_subprocess_context(),
                                opts)

    cover_ctx.cleanup()
    if not killed and not opts.test_glob:
      if not cover_ctx.report(opts.verbose):
        sys.exit(2)

    sys.exit(error or killed)
  except KeyboardInterrupt:
    pass
