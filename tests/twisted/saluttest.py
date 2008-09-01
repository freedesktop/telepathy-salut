
"""
Infrastructure code for testing Gabble by pretending to be a Jabber server.
"""

import base64
import os
import sha
import sys

import servicetest
import twisted
from twisted.internet import reactor

import dbus

def make_connection(bus, event_func, params=None):
    default_params = {
        'account': 'test@localhost/Resource',
        'password': 'pass',
        'resource': 'Resource',
        'server': 'localhost',
        'port': dbus.UInt32(4242),
        }

    if params:
        default_params.update(params)

    return servicetest.make_connection(bus, event_func, 'salut',
        'local-xmpp', default_params)

def install_colourer():
    def red(s):
        return '\x1b[31m%s\x1b[0m' % s

    def green(s):
        return '\x1b[32m%s\x1b[0m' % s

    patterns = {
        'handled': green,
        'not handled': red,
        }

    class Colourer:
        def __init__(self, fh, patterns):
            self.fh = fh
            self.patterns = patterns

        def write(self, s):
            f = self.patterns.get(s, lambda x: x)
            self.fh.write(f(s))

    sys.stdout = Colourer(sys.stdout, patterns)
    return sys.stdout


def exec_test_deferred (fun, params, protocol=None, timeout=None):
    colourer = None

    if sys.stdout.isatty():
        colourer = install_colourer()

    queue = servicetest.IteratingEventQueue(timeout)
    queue.verbose = (
        os.environ.get('CHECK_TWISTED_VERBOSE', '') != ''
        or '-v' in sys.argv)

    bus = dbus.SessionBus()
    conn = make_connection(bus, queue.append, params)
    (stream, port) = make_stream(queue.append, protocol=protocol)

    error = None

    try:
        fun(queue, bus, conn, stream)
    except Exception, e:
        import traceback
        traceback.print_exc()
        error = e

    try:
        if colourer:
          sys.stdout = colourer.fh
        d = port.stopListening()
        if error is None:
            d.addBoth((lambda *args: reactor.crash()))
        else:
            # please ignore the POSIX behind the curtain
            d.addBoth((lambda *args: os._exit(1)))

        conn.Disconnect()
        # second call destroys object
        conn.Disconnect()
    except dbus.DBusException, e:
        pass

def exec_test(fun, params=None, protocol=None, timeout=None):
  reactor.callWhenRunning (exec_test_deferred, fun, params, protocol, timeout)
  reactor.run()