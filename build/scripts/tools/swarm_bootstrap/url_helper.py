# Copyright (c) 2012 The Chromium Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

"""A helper script for wrapping url calls."""

import logging
import math
import os
import random
import time
import urllib
import urllib2
import urlparse


COUNT_KEY = 'UrlOpenAttempt'
RESULT_STRING_KEY = 'result_output'

# The index of the query elements from urlparse.
QUERY_INDEX = 4


def UrlOpen(url, data=None, max_tries=5, wait_duration=None, method='POST'):
  """Attempts to open the given url multiple times.

  UrlOpen will attempt to open the the given url several times, stopping
  if it succeeds at reaching the url. It also includes an additional data pair
  in the data that is sent to indicate how many times it has attempted to
  connect so far.

  Args:
    url: The url to open.
    data: The unencoded data to send to the url. This must be a mapping object.
    max_tries: The maximum number of times to try sending this data. Must be
        greater than 0.
    wait_duration: The number of seconds to wait between successive attempts.
        This must be greater than or equal to 0. If no value is given then a
        random value between 0.1 and 10 will be chosen each time (with
        exponential back off to give later retries a longer wait).
    method: Indicates if the request should be a GET or POST request.

  Returns:
    The reponse from the url contacted. If it failed to connect or is given
    invalid arguments, then it returns None.
  """

  if max_tries <= 0:
    logging.error('Invalid number of tries, %d, passed in.', max_tries)
    return None

  if wait_duration and wait_duration < 0:
    logging.error('Invalid wait duration, %d, passed in.', wait_duration)
    return None

  data = data or {}

  if COUNT_KEY in data:
    logging.error('%s already existed in the data passed into UlrOpen. It '
                  'would be overwritten. Aborting UrlOpen', COUNT_KEY)
    return None

  url_response = None
  for attempt in range(max_tries):
    data[COUNT_KEY] = attempt
    try:
      encoded_data = urllib.urlencode(data)
      if method == 'POST':
        # Simply specifying data to urlopen makes it a POST.
        url_response = urllib2.urlopen(url, encoded_data).read()
      else:
        url_parts = list(urlparse.urlparse(url))
        url_parts[QUERY_INDEX] = encoded_data
        url = urlparse.urlunparse(url_parts)
        url_response = urllib2.urlopen(url).read()
    except urllib2.HTTPError as e:
      # An HTTPError means we reached the server, so don't retry.
      logging.error('Able to connect to %s but an exception was thrown.\n%s',
                    url, e)
      return None
    except urllib2.URLError as e:
      logging.info(
          'Unable to open url %s on attempt %d.\nException: %s',
          url, attempt, e)

      if wait_duration is None:
        duration = random.random() * 3 + math.pow(1.5, (attempt + 1))
        duration = min(10, max(0.1, duration))
      else:
        duration = wait_duration

      # Only sleep if we are going to try again.
      if attempt != max_tries - 1:
        time.sleep(duration)

    if url_response is not None:
      logging.info('Opened given url, %s, and got response:\n%s', url,
                   url_response)
      return url_response

  logging.error('Unable to open given url, %s, after %d attempts.',
                url, max_tries)
  return None


def DownloadFile(local_file, url):
  """Downloads the data from the given url and saves it in the local_file.

  Args:
    local_file: Where to save the data downloaded from the url.
    url: Where to fetch the data from.

  Returns:
    True if the file is successfully downloaded.
  """
  local_file = os.path.abspath(local_file)

  url_data = UrlOpen(url, method='GET')

  if url_data is None:
    return False

  try:
    with open(local_file, 'wb') as f:
      f.write(url_data)
  except IOError as e:
    logging.error('Failed to write to %s\n%s', local_file, e)
    return False

  return True
