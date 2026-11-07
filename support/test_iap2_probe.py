"""Adversarial stream/handshake fixtures; no Bluetooth hardware or MFi secrets."""
import os
import shlex
import socket
import struct
import subprocess
import time
import unittest

DETECT = bytes.fromhex('ff550200ee10')
PARAMETERS = bytes.fromhex('0104100003e8006403010a0001')


def frame(flags, sequence, ack=0, session=0, payload=b''):
    size = 9 + (len(payload) + 1 if payload else 0)
    header = struct.pack('>HHBBBB', 0xff5a, size, flags, sequence, ack, session)
    header += bytes([-sum(header) & 255])
    return header + (payload + bytes([-sum(payload) & 255]) if payload else b'')


def control(message_id, payload=b''):
    return struct.pack('>HHH', 0x4040, 6 + len(payload), message_id) + payload


class ProbeTests(unittest.TestCase):
    def setUp(self):
        self.peer, connection = socket.socketpair()
        self.peer.settimeout(5)
        command = shlex.split(os.environ['IAP2_TEST_COMMAND'])
        self.process = subprocess.Popen(command + [str(connection.fileno())], pass_fds=(connection.fileno(),),
                                        stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        connection.close()
        self.addCleanup(self.cleanup)

    def cleanup(self):
        self.peer.close()
        if self.process.poll() is None:
            self.process.kill()
        self.process.communicate()

    def read(self, count):
        data = b''
        while len(data) < count:
            part = self.peer.recv(count - len(data))
            self.assertTrue(part, 'probe closed transport unexpectedly')
            data += part
        return data

    def packet(self):
        header = self.read(9)
        self.assertEqual(bytes.fromhex('ff5a'), header[:2])
        self.assertEqual(0, sum(header) & 255)
        size = int.from_bytes(header[2:4], 'big')
        body = self.read(size - 9)
        self.assertEqual(0, sum(body) & 255)
        return header, body[:-1] if body else b''

    def detect(self):
        self.assertEqual(DETECT, self.read(6))
        # A real RFCOMM stream may split the support marker arbitrarily.
        self.peer.sendall(b'noise' + DETECT[:2])
        self.peer.sendall(DETECT[2:])

    def link(self, session=10):
        self.detect()
        header, parameters = self.packet()
        self.assertEqual(0x80, header[4])
        self.assertEqual(PARAMETERS, parameters)
        remote = PARAMETERS[:-3] + bytes([session, 0, 1])
        self.peer.sendall(frame(0xc0, 41, payload=remote))
        header, body = self.packet()
        self.assertEqual(0x40, header[4])
        self.assertEqual(41, header[6])
        self.assertFalse(body)

    def result(self, expected):
        output, errors = self.process.communicate(timeout=5)
        self.assertEqual(expected, self.process.returncode, output.decode() + errors.decode())
        return output.decode()

    def test_certificate_request_stops_at_real_missing_provider(self):
        self.link()
        self.peer.sendall(frame(0x40, 42, session=10, payload=control(0xaa00)))
        header, _ = self.packet()
        self.assertEqual(42, header[6])
        output = self.result(20)
        self.assertIn('authentication_request\tOK', output)
        self.assertIn('authentication_provider\tBLOCKED', output)

    def test_control_message_split_across_packets(self):
        self.link()
        message = control(0xaa00)
        self.peer.sendall(frame(0x40, 42, session=10, payload=message[:3]))
        self.packet()
        self.peer.sendall(frame(0x40, 43, session=10, payload=message[3:]))
        self.packet()
        self.result(20)

    def test_negotiated_control_session_and_duplicate_packet(self):
        self.link(session=37)
        self.peer.sendall(frame(0x40, 41, session=37, payload=control(0xaa00)))
        header, _ = self.packet()
        self.assertEqual(41, header[6])
        self.peer.sendall(frame(0x40, 42, session=37, payload=control(0xaa00)))
        self.packet()
        self.result(20)

    def test_identification_is_not_mislabeled_as_authentication(self):
        self.link()
        self.peer.sendall(frame(0x40, 42, session=10, payload=control(0x1d00)))
        self.packet()
        output = self.result(21)
        self.assertIn('identification_request\tOK', output)
        self.assertNotIn('authentication_request\tOK', output)

    def test_control_payload_not_exposed_in_logs(self):
        self.link()
        secret = b'DO_NOT_LOG_CHALLENGE_CONTENT'
        self.peer.sendall(frame(0x40, 42, session=10, payload=control(0xaa02, secret)))
        self.packet()
        self.peer.close()
        output = self.result(23)
        self.assertNotIn(secret.decode(), output)

    def test_header_checksum_rejected(self):
        self.detect()
        self.packet()
        packet = bytearray(frame(0xc0, 41, payload=PARAMETERS))
        packet[8] ^= 1
        self.peer.sendall(packet)
        self.assertIn('header checksum', self.result(24))

    def test_oversized_length_rejected_without_waiting_for_payload(self):
        self.detect()
        self.packet()
        header = struct.pack('>HHBBBB', 0xff5a, 65535, 0xc0, 41, 0, 0)
        self.peer.sendall(header + bytes([-sum(header) & 255]))
        self.assertIn('length outside', self.result(24))

    def test_payload_checksum_rejected(self):
        self.link()
        packet = bytearray(frame(0x40, 42, session=10, payload=control(0xaa00)))
        packet[-1] ^= 1
        self.peer.sendall(packet)
        self.assertIn('payload checksum', self.result(24))

    def test_invalid_control_length_rejected(self):
        self.link()
        self.peer.sendall(frame(0x40, 42, session=10, payload=bytes.fromhex('40400005aa00')))
        self.packet()
        self.assertIn('control message length', self.result(24))

    def test_truncated_packet_disconnect(self):
        self.link()
        packet = frame(0x40, 42, session=10, payload=control(0xaa00))
        self.peer.sendall(packet[:-2])
        self.peer.close()
        self.assertIn('DISCONNECTED', self.result(23))

    def test_peer_reset(self):
        self.link()
        self.peer.sendall(frame(0x10, 42))
        self.assertIn('peer reset', self.result(25))

    def test_detection_timeout_is_bounded(self):
        started = time.monotonic()
        self.assertIn('iap2_detection\tTIMEOUT', self.result(22))
        self.assertLess(time.monotonic() - started, 4)

    def test_link_timeout_does_not_claim_control_session(self):
        self.detect()
        self.packet()
        output = self.result(22)
        self.assertIn('iap2_link\tTIMEOUT', output)
        self.assertNotIn('iap2_link\tOK', output)


if __name__ == '__main__':
    unittest.main()
