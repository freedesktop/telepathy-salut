import httplib
import urlparse
import dbus
import socket
import md5
import avahi
import BaseHTTPServer
import urllib

from saluttest import exec_test
from avahitest import AvahiAnnouncer, AvahiListener
from avahitest import get_host_name

from xmppstream import setup_stream_listener, connect_to_stream
from servicetest import make_channel_proxy, EventPattern

from twisted.words.xish import domish, xpath

from dbus import PROPERTIES_IFACE

CONNECTION_INTERFACE_REQUESTS = 'org.freedesktop.Telepathy.Connection.Interface.Requests'
CHANNEL_INTERFACE ='org.freedesktop.Telepathy.Channel'
CHANNEL_TYPE_FILE_TRANSFER = 'org.freedesktop.Telepathy.Channel.Type.FileTransfer.DRAFT'

HT_CONTACT = 1
HT_CONTACT_LIST = 3
TEXT_MESSAGE_TYPE_NORMAL = dbus.UInt32(0)

FT_STATE_NONE = 0
FT_STATE_PENDING = 1
FT_STATE_ACCEPTED = 2
FT_STATE_OPEN = 3
FT_STATE_COMPLETED = 4
FT_STATE_CANCELLED = 5

FT_STATE_CHANGE_REASON_NONE = 0
FT_STATE_CHANGE_REASON_REQUESTED = 1

FILE_HASH_TYPE_NONE = 0
FILE_HASH_TYPE_MD5 = 1

SOCKET_ADDRESS_TYPE_UNIX = 0
SOCKET_ADDRESS_TYPE_IPV4 = 2

SOCKET_ACCESS_CONTROL_LOCALHOST = 0

# File to Offer
FILE_DATA = "What a nice file"
FILE_SIZE = len(FILE_DATA)
FILE_NAME = 'The foo.txt'
FILE_CONTENT_TYPE = 'text/plain'
FILE_DESCRIPTION = 'A nice file to test'
FILE_HASH_TYPE = FILE_HASH_TYPE_MD5
m = md5.new()
m.update(FILE_DATA)
FILE_HASH = m.hexdigest()

def test(q, bus, conn):
    conn.Connect()
    q.expect('dbus-signal', signal='StatusChanged', args=[0L, 0L])
    basic_txt = { "txtvers": "1", "status": "avail" }

    self_handle = conn.GetSelfHandle()
    self_handle_name =  conn.InspectHandles(HT_CONTACT, [self_handle])[0]

    contact_name = "test-file-sender@" + get_host_name()
    listener, port = setup_stream_listener(q, contact_name)

    AvahiAnnouncer(contact_name, "_presence._tcp", port, basic_txt)

    publish_handle = conn.RequestHandles(HT_CONTACT_LIST, ["publish"])[0]
    publish = conn.RequestChannel(
        "org.freedesktop.Telepathy.Channel.Type.ContactList",
        HT_CONTACT_LIST, publish_handle, False)

    handle = 0
    # Wait until the record shows up in publish
    while handle == 0:
        e = q.expect('dbus-signal', signal='MembersChanged', path=publish)
        for h in e.args[1]:
            name = conn.InspectHandles(HT_CONTACT, [h])[0]
            if name == contact_name:
                handle = h

    requests_iface = dbus.Interface(conn, CONNECTION_INTERFACE_REQUESTS)

    # Create a connection to send the FT stanza
    AvahiListener(q).listen_for_service("_presence._tcp")
    e = q.expect('service-added', name = self_handle_name,
        protocol = avahi.PROTO_INET)
    service = e.service
    service.resolve()

    e = q.expect('service-resolved', service = service)

    outbound = connect_to_stream(q, contact_name,
        self_handle_name, str(e.pt), e.port)

    e = q.expect('connection-result')
    assert e.succeeded, e.reason
    e = q.expect('stream-opened', connection = outbound)

    # Setup the HTTP server
    httpd = BaseHTTPServer.HTTPServer(('', 0), HTTPHandler)

    # connected to Salut, now send the IQ
    iq = domish.Element((None, 'iq'))
    iq['to'] = self_handle_name
    iq['from'] = contact_name
    iq['type'] = 'set'
    iq['id'] = 'gibber-file-transfer-0'
    query = iq.addElement(('jabber:iq:oob', 'query'))
    url = 'http://127.0.0.1:%u/gibber-file-transfer-0/%s' % (httpd.server_port, urllib.quote(FILE_NAME))
    url_node = query.addElement('url', content=url)
    url_node['type'] = 'file'
    url_node['size'] = str(FILE_SIZE)
    url_node['mimeType'] = FILE_CONTENT_TYPE
    query.addElement('desc', content=FILE_DESCRIPTION)
    outbound.send(iq)

    e =q.expect('dbus-signal', signal='NewChannels')
    channels = e.args[0]
    assert len(channels) == 1
    path, props = channels[0]

    # check channel properties
    # org.freedesktop.Telepathy.Channel D-Bus properties
    assert props[CHANNEL_INTERFACE + '.ChannelType'] == CHANNEL_TYPE_FILE_TRANSFER
    assert props[CHANNEL_INTERFACE + '.Interfaces'] == []
    assert props[CHANNEL_INTERFACE + '.TargetHandle'] == handle
    assert props[CHANNEL_INTERFACE + '.TargetID'] == contact_name
    assert props[CHANNEL_INTERFACE + '.TargetHandleType'] == HT_CONTACT
    assert props[CHANNEL_INTERFACE + '.Requested'] == False
    assert props[CHANNEL_INTERFACE + '.InitiatorHandle'] == handle
    assert props[CHANNEL_INTERFACE + '.InitiatorID'] == contact_name

    # org.freedesktop.Telepathy.Channel.Type.FileTransfer D-Bus properties
    assert props[CHANNEL_TYPE_FILE_TRANSFER + '.State'] == FT_STATE_PENDING
    assert props[CHANNEL_TYPE_FILE_TRANSFER + '.ContentType'] == FILE_CONTENT_TYPE
    assert props[CHANNEL_TYPE_FILE_TRANSFER + '.Filename'] == FILE_NAME
    assert props[CHANNEL_TYPE_FILE_TRANSFER + '.Size'] == FILE_SIZE
    # FT's protocol doesn't allow us the send the hash info
    assert props[CHANNEL_TYPE_FILE_TRANSFER + '.ContentHashType'] == FILE_HASH_TYPE_NONE
    assert props[CHANNEL_TYPE_FILE_TRANSFER + '.ContentHash'] == ''
    assert props[CHANNEL_TYPE_FILE_TRANSFER + '.Description'] == FILE_DESCRIPTION
    # FT's protocol doesn't allow us the send the date info
    assert props[CHANNEL_TYPE_FILE_TRANSFER + '.Date'] == 0
    assert props[CHANNEL_TYPE_FILE_TRANSFER + '.AvailableSocketTypes'] == \
        {SOCKET_ADDRESS_TYPE_UNIX: [SOCKET_ACCESS_CONTROL_LOCALHOST]}
    assert props[CHANNEL_TYPE_FILE_TRANSFER + '.TransferredBytes'] == 0
    assert props[CHANNEL_TYPE_FILE_TRANSFER + '.InitialOffset'] == 0

    channel = make_channel_proxy(conn, path, 'Channel')
    ft_channel = make_channel_proxy(conn, path, 'Channel.Type.FileTransfer.DRAFT')
    ft_props = dbus.Interface(bus.get_object(conn.object.bus_name, path), PROPERTIES_IFACE)

    address = ft_channel.AcceptFile(SOCKET_ADDRESS_TYPE_UNIX, SOCKET_ACCESS_CONTROL_LOCALHOST, "", 5)

    e = q.expect('dbus-signal', signal='FileTransferStateChanged')
    state, reason = e.args
    assert state == FT_STATE_ACCEPTED
    assert reason == FT_STATE_CHANGE_REASON_REQUESTED

    e = q.expect('dbus-signal', signal='InitialOffsetDefined')
    offset = e.args[0]
    # We don't support resume
    assert offset == 0

    e = q.expect('dbus-signal', signal='FileTransferStateChanged')
    state, reason = e.args
    assert state == FT_STATE_OPEN
    assert reason == FT_STATE_CHANGE_REASON_NONE

    # Connect to Salut's socket
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.connect(address)

    httpd.handle_request()

    # Receiver inform us he finished to download the file
    q.expect('stream-iq', iq_type='result')

    # Read the file from Salut's socket
    data = ''
    read = 0
    while read < FILE_SIZE:
        data += s.recv(FILE_SIZE - read)
        read = len(data)
    assert data == FILE_DATA

    e = q.expect('dbus-signal', signal='TransferredBytesChanged')
    count = e.args[0]
    while count < FILE_SIZE:
        # Catch TransferredBytesChanged until we transfered all the data
        e = q.expect('dbus-signal', signal='TransferredBytesChanged')
        count = e.args[0]

    e = q.expect('dbus-signal', signal='FileTransferStateChanged')
    state, reason = e.args
    assert state == FT_STATE_COMPLETED
    assert reason == FT_STATE_CHANGE_REASON_NONE

    channel.Close()
    q.expect('dbus-signal', signal='Closed')

    # now send a file. We'll reuse the same XMPP connection
    path, props = requests_iface.CreateChannel({
        CHANNEL_INTERFACE + '.ChannelType': CHANNEL_TYPE_FILE_TRANSFER,
        CHANNEL_INTERFACE + '.TargetHandleType': HT_CONTACT,
        CHANNEL_INTERFACE + '.TargetHandle': handle,
        CHANNEL_TYPE_FILE_TRANSFER + '.ContentType': FILE_CONTENT_TYPE,
        CHANNEL_TYPE_FILE_TRANSFER + '.Filename': FILE_NAME,
        CHANNEL_TYPE_FILE_TRANSFER + '.Size': FILE_SIZE,
        CHANNEL_TYPE_FILE_TRANSFER + '.ContentHashType': FILE_HASH_TYPE,
        CHANNEL_TYPE_FILE_TRANSFER + '.ContentHash': FILE_HASH,
        CHANNEL_TYPE_FILE_TRANSFER + '.Description': FILE_DESCRIPTION,
        CHANNEL_TYPE_FILE_TRANSFER + '.Date':  1225278834,
        CHANNEL_TYPE_FILE_TRANSFER + '.InitialOffset': 0,
        })

    channel = make_channel_proxy(conn, path, 'Channel')
    ft_channel = make_channel_proxy(conn, path, 'Channel.Type.FileTransfer.DRAFT')
    ft_props = dbus.Interface(bus.get_object(conn.object.bus_name, path), PROPERTIES_IFACE)

    iq_event = q.expect('stream-iq')

    assert iq_event.iq_type == 'set'
    assert iq_event.connection == outbound
    iq = iq_event.stanza
    assert iq['to'] == contact_name
    query = iq.firstChildElement()
    assert query.uri == 'jabber:iq:oob'
    url_node = xpath.queryForNodes("/iq/query/url",  iq)[0]
    assert url_node['type'] == 'file'
    assert url_node['size'] == str(FILE_SIZE)
    assert url_node['mimeType'] == FILE_CONTENT_TYPE
    url = url_node.children[0]
    _, host, file, _, _, _ = urlparse.urlparse(url)
    urllib.unquote(file) == FILE_NAME
    desc_node = xpath.queryForNodes("/iq/query/desc",  iq)[0]
    desc = desc_node.children[0]
    assert desc == FILE_DESCRIPTION

    address = ft_channel.ProvideFile(SOCKET_ADDRESS_TYPE_UNIX, SOCKET_ACCESS_CONTROL_LOCALHOST, "")

    # state is still Pending as remote didn't accept the transfer yet
    state = ft_props.Get(CHANNEL_TYPE_FILE_TRANSFER, 'State')
    assert state == FT_STATE_PENDING

    # Connect HTTP client to the CM and request the file
    http = httplib.HTTPConnection(host)
    http.request('GET', file)

    e = q.expect('dbus-signal', signal='InitialOffsetDefined')
    offset = e.args[0]
    # We don't support resume
    assert offset == 0

    # Channel is open. We can start to send the file
    e = q.expect('dbus-signal', signal='FileTransferStateChanged')
    state, reason = e.args
    assert state == FT_STATE_OPEN
    assert reason == FT_STATE_CHANGE_REASON_NONE

    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.connect(address)
    s.send(FILE_DATA)

    e = q.expect('dbus-signal', signal='TransferredBytesChanged')

    count = e.args[0]
    while count < FILE_SIZE:
        # Catch TransferredBytesChanged until we transfered all the data
        e = q.expect('dbus-signal', signal='TransferredBytesChanged')
        count = e.args[0]

    response = http.getresponse()
    assert (response.status, response.reason) == (200, 'OK')
    data = response.read(FILE_SIZE)
    # Did we received the right file?
    assert data == FILE_DATA

    # Inform sender that we received all the file from the OOB transfer
    reply = domish.Element(('', 'iq'))
    reply['to'] = iq['from']
    reply['from'] = iq['to']
    reply['type'] = 'result'
    reply['id'] = iq['id']
    outbound.send(reply)

    e = q.expect('dbus-signal', signal='FileTransferStateChanged')
    state, reason = e.args
    assert state == FT_STATE_COMPLETED
    assert reason == FT_STATE_CHANGE_REASON_NONE

    channel.Close()
    q.expect('dbus-signal', signal='Closed')

    conn.Disconnect()
    q.expect('dbus-signal', signal='StatusChanged', args=[2L, 1L])

class HTTPHandler(BaseHTTPServer.BaseHTTPRequestHandler):
    def do_GET(self):
        # is that the right file ?
        filename = self.path.rsplit('/', 2)[-1]
        assert filename == urllib.quote(FILE_NAME)

        self.send_response(200)
        self.send_header('Content-type', FILE_CONTENT_TYPE)
        self.end_headers()
        self.wfile.write(FILE_DATA)

if __name__ == '__main__':
    exec_test(test)