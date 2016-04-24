import io
import unittest
import urllib.robotparser
from collections import namedtuple
from test import support
from http.server import BaseHTTPRequestHandler, HTTPServer
try:
    import threading
except ImportError:
    threading = None


class RobotTestCase(unittest.TestCase):
    def __init__(self, index=None, parser=None, url=None, good=None,
                 agent=None, request_rate=None, crawl_delay=None):
        # workaround to make unittest discovery work (see #17066)
        if not isinstance(index, int):
            return
        unittest.TestCase.__init__(self)
        if good:
            self.str = "RobotTest(%d, good, %s)" % (index, url)
        else:
            self.str = "RobotTest(%d, bad, %s)" % (index, url)
        self.parser = parser
        self.url = url
        self.good = good
        self.agent = agent
        self.request_rate = request_rate
        self.crawl_delay = crawl_delay

    def runTest(self):
        if isinstance(self.url, tuple):
            agent, url = self.url
        else:
            url = self.url
            agent = self.agent
        if self.good:
            self.assertTrue(self.parser.can_fetch(agent, url))
            self.assertEqual(self.parser.crawl_delay(agent), self.crawl_delay)
            # if we have actual values for request rate
            if self.request_rate and self.parser.request_rate(agent):
                self.assertEqual(
                    self.parser.request_rate(agent).requests,
                    self.request_rate.requests
                )
                self.assertEqual(
                    self.parser.request_rate(agent).seconds,
                    self.request_rate.seconds
                )
            self.assertEqual(self.parser.request_rate(agent), self.request_rate)
        else:
            self.assertFalse(self.parser.can_fetch(agent, url))

    def __str__(self):
        return self.str

tests = unittest.TestSuite()

def RobotTest(index, robots_txt, good_urls, bad_urls,
              request_rate, crawl_delay, agent="test_robotparser"):

    lines = io.StringIO(robots_txt).readlines()
    parser = urllib.robotparser.RobotFileParser()
    parser.parse(lines)
    for url in good_urls:
        tests.addTest(RobotTestCase(index, parser, url, 1, agent,
                      request_rate, crawl_delay))
    for url in bad_urls:
        tests.addTest(RobotTestCase(index, parser, url, 0, agent,
                      request_rate, crawl_delay))

# Examples from http://www.robotstxt.org/wc/norobots.html (fetched 2002)

# 1.
doc = """
User-agent: *
Disallow: /cyberworld/map/ # This is an infinite virtual URL space
Disallow: /tmp/ # these will soon disappear
Disallow: /foo.html
"""

good = ['/','/test.html']
bad = ['/cyberworld/map/index.html','/tmp/xxx','/foo.html']
request_rate = None
crawl_delay = None

RobotTest(1, doc, good, bad, request_rate, crawl_delay)

# 2.
doc = """
# robots.txt for http://www.example.com/

User-agent: *
Crawl-delay: 1
Request-rate: 3/15
Disallow: /cyberworld/map/ # This is an infinite virtual URL space

# Cybermapper knows where to go.
User-agent: cybermapper
Disallow:

"""

good = ['/','/test.html',('cybermapper','/cyberworld/map/index.html')]
bad = ['/cyberworld/map/index.html']
request_rate = None  # The parameters should be equal to None since they
crawl_delay = None   # don't apply to the cybermapper user agent

RobotTest(2, doc, good, bad, request_rate, crawl_delay)

# 3.
doc = """
# go away
User-agent: *
Disallow: /
"""

good = []
bad = ['/cyberworld/map/index.html','/','/tmp/']
request_rate = None
crawl_delay = None

RobotTest(3, doc, good, bad, request_rate, crawl_delay)

# Examples from http://www.robotstxt.org/wc/norobots-rfc.html (fetched 2002)

# 4.
doc = """
User-agent: figtree
Crawl-delay: 3
Request-rate: 9/30
Disallow: /tmp
Disallow: /a%3cd.html
Disallow: /a%2fb.html
Disallow: /%7ejoe/index.html
"""

good = [] # XFAIL '/a/b.html'
bad = ['/tmp','/tmp.html','/tmp/a.html',
       '/a%3cd.html','/a%3Cd.html','/a%2fb.html',
       '/~joe/index.html'
       ]

request_rate = namedtuple('req_rate', 'requests seconds')
request_rate.requests = 9
request_rate.seconds = 30
crawl_delay = 3
request_rate_bad = None  # not actually tested, but we still need to parse it
crawl_delay_bad = None  # in order to accommodate the input parameters


RobotTest(4, doc, good, bad, request_rate, crawl_delay, 'figtree' )
RobotTest(5, doc, good, bad, request_rate_bad, crawl_delay_bad,
          'FigTree Robot libwww-perl/5.04')

# 6.
doc = """
User-agent: *
Disallow: /tmp/
Disallow: /a%3Cd.html
Disallow: /a/b.html
Disallow: /%7ejoe/index.html
Crawl-delay: 3
Request-rate: 9/banana
"""

good = ['/tmp',] # XFAIL: '/a%2fb.html'
bad = ['/tmp/','/tmp/a.html',
       '/a%3cd.html','/a%3Cd.html',"/a/b.html",
       '/%7Ejoe/index.html']
crawl_delay = 3
request_rate = None  # since request rate has invalid syntax, return None

RobotTest(6, doc, good, bad, None, None)

# From bug report #523041

# 7.
doc = """
User-Agent: *
Disallow: /.
Crawl-delay: pears
"""

good = ['/foo.html']
bad = []  # bug report says "/" should be denied, but that is not in the RFC

crawl_delay = None  # since crawl delay has invalid syntax, return None
request_rate = None

RobotTest(7, doc, good, bad, crawl_delay, request_rate)

# From Google: http://www.google.com/support/webmasters/bin/answer.py?hl=en&answer=40364

# 8.
doc = """
User-agent: Googlebot
Allow: /folder1/myfile.html
Disallow: /folder1/
Request-rate: whale/banana
"""

good = ['/folder1/myfile.html']
bad = ['/folder1/anotherfile.html']
crawl_delay = None
request_rate = None  # invalid syntax, return none

RobotTest(8, doc, good, bad, crawl_delay, request_rate, agent="Googlebot")

# 9.  This file is incorrect because "Googlebot" is a substring of
#     "Googlebot-Mobile", so test 10 works just like test 9.
doc = """
User-agent: Googlebot
Disallow: /

User-agent: Googlebot-Mobile
Allow: /
"""

good = []
bad = ['/something.jpg']

RobotTest(9, doc, good, bad, None, None, agent="Googlebot")

good = []
bad = ['/something.jpg']

RobotTest(10, doc, good, bad, None, None, agent="Googlebot-Mobile")

# 11.  Get the order correct.
doc = """
User-agent: Googlebot-Mobile
Allow: /

User-agent: Googlebot
Disallow: /
"""

good = []
bad = ['/something.jpg']

RobotTest(11, doc, good, bad, None, None, agent="Googlebot")

good = ['/something.jpg']
bad = []

RobotTest(12, doc, good, bad, None, None, agent="Googlebot-Mobile")


# 13.  Google also got the order wrong in #8.  You need to specify the
#      URLs from more specific to more general.
doc = """
User-agent: Googlebot
Allow: /folder1/myfile.html
Disallow: /folder1/
"""

good = ['/folder1/myfile.html']
bad = ['/folder1/anotherfile.html']

RobotTest(13, doc, good, bad, None, None, agent="googlebot")


# 14. For issue #6325 (query string support)
doc = """
User-agent: *
Disallow: /some/path?name=value
"""

good = ['/some/path']
bad = ['/some/path?name=value']

RobotTest(14, doc, good, bad, None, None)

# 15. For issue #4108 (obey first * entry)
doc = """
User-agent: *
Disallow: /some/path

User-agent: *
Disallow: /another/path
"""

good = ['/another/path']
bad = ['/some/path']

RobotTest(15, doc, good, bad, None, None)

# 16. Empty query (issue #17403). Normalizing the url first.
doc = """
User-agent: *
Allow: /some/path?
Disallow: /another/path?
"""

good = ['/some/path?']
bad = ['/another/path?']

RobotTest(16, doc, good, bad, None, None)


class RobotHandler(BaseHTTPRequestHandler):

    def do_GET(self):
        self.send_error(403, "Forbidden access")

    def log_message(self, format, *args):
        pass


@unittest.skipUnless(threading, 'threading required for this test')
class PasswordProtectedSiteTestCase(unittest.TestCase):

    def setUp(self):
        self.server = HTTPServer((support.HOST, 0), RobotHandler)

        self.t = threading.Thread(
            name='HTTPServer serving',
            target=self.server.serve_forever,
            # Short poll interval to make the test finish quickly.
            # Time between requests is short enough that we won't wake
            # up spuriously too many times.
            kwargs={'poll_interval':0.01})
        self.t.daemon = True  # In case this function raises.
        self.t.start()

    def tearDown(self):
        self.server.shutdown()
        self.t.join()
        self.server.server_close()

    def runTest(self):
        self.testPasswordProtectedSite()

    def testPasswordProtectedSite(self):
        addr = self.server.server_address
        url = 'http://' + support.HOST + ':' + str(addr[1])
        robots_url = url + "/robots.txt"
        parser = urllib.robotparser.RobotFileParser()
        parser.set_url(url)
        parser.read()
        self.assertFalse(parser.can_fetch("*", robots_url))

    def __str__(self):
        return '%s' % self.__class__.__name__

class NetworkTestCase(unittest.TestCase):

    @unittest.skip('does not handle the gzip encoding delivered by pydotorg')
    def testPythonOrg(self):
        support.requires('network')
        with support.transient_internet('www.python.org'):
            parser = urllib.robotparser.RobotFileParser(
                "http://www.python.org/robots.txt")
            parser.read()
            self.assertTrue(
                parser.can_fetch("*", "http://www.python.org/robots.txt"))

def load_tests(loader, suite, pattern):
    suite = unittest.makeSuite(NetworkTestCase)
    suite.addTest(tests)
    suite.addTest(PasswordProtectedSiteTestCase())
    return suite

if __name__=='__main__':
    unittest.main()
