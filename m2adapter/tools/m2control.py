# adapted from mongrel2's manual

import sys
import zmq
import tnetstring
from pprint import pprint
  
CTX = zmq.Context()
addr = sys.argv[1]
  
ctl = CTX.socket(zmq.REQ)
  
print 'CONNECTING'
ctl.connect(addr)
  
while True:
	cmd = raw_input('> ')
	# will only work with simple commands that have no arguments
	ctl.send(tnetstring.dumps([cmd, {}]))

	resp = ctl.recv()

	pprint(tnetstring.loads(resp))

ctl.close()
