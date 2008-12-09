import dbus

from saluttest import exec_test
from file_transfer_helper import ReceiveFileTest, SOCKET_ADDRESS_TYPE_UNIX,\
    SOCKET_ACCESS_CONTROL_LOCALHOST

class ReceiveFileAndSenderDisconnectTest(ReceiveFileTest):
    def accept_file(self):
        # The sender of the file disconnects

        self.contact_service.stop()

        self.q.expect('dbus-signal', signal='FileTransferStateChanged')

        # We can't accept the transfer now
        try:
            self.ft_channel.AcceptFile(SOCKET_ADDRESS_TYPE_UNIX,
                SOCKET_ACCESS_CONTROL_LOCALHOST, "", 0)
        except dbus.DBusException, e:
            assert e.get_dbus_name() == 'org.freedesktop.Telepathy.Errors.NotAvailable'
        else:
            assert False

        self.close_channel()

        # stop the test
        return True

if __name__ == '__main__':
    test = ReceiveFileAndSenderDisconnectTest()
    exec_test(test.test)
