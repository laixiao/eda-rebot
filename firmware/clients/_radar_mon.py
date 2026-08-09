import sys, time
sys.path.insert(0, 'C:/Users/25150/Desktop/eda-rebot/firmware/clients')
from robot_api import RobotApi

HOST = 'http://192.168.137.185'
b = RobotApi(HOST)

# ensure powered
print('ensure power ON ->', b.radar_power(True).get('ok'))

for i in range(6):
    time.sleep(10)
    d = b.radar()
    print('t+%ds link=%s rx=%s rxBytes=%s rxLevel=%s txLevel=%s f59=%s f5A=%s ver=%s peek=%s'
          % ((i+1)*10, d.get('link'), d.get('rx'), d.get('rxBytes'), d.get('rxLevel'),
             d.get('txLevel'), d.get('frames59'), d.get('frames5A'), d.get('version'),
             d.get('rxPeekHex')))

# force version query as a prod
print('cmd=version ->', b._call('/api/radar', {'cmd': 'version'}, 'POST'))
time.sleep(3)
d = b.radar()
print('after ver: link=%s rxBytes=%s rxLevel=%s f59=%s ver=%s' % (d.get('link'), d.get('rxBytes'),
      d.get('rxLevel'), d.get('frames59'), d.get('version')))