import sys
import socket
import threading
import subprocess
from pubcontrol import Item
from gripcontrol import GripPubControl, HttpStreamFormat

pub = GripPubControl({'control_uri': 'http://localhost:5561'})

def publish_worker():
	sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
	sock.bind(('127.0.0.1', 5004))
	while True:
		data, addr = sock.recvfrom(65536)
		pub.publish('music', Item(HttpStreamFormat(data)))

thread = threading.Thread(target=publish_worker)
thread.daemon = True
thread.start()

subprocess.check_call(['gst-launch-1.0', 'filesrc', 'location=%s' % sys.argv[1], '!', 'decodebin', '!', 'queue', '!', 'lamemp3enc', '!', 'udpsink', 'clients=localhost:5004'])
