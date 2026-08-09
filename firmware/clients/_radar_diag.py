import sys, time
sys.path.insert(0, 'C:/Users/25150/Desktop/eda-rebot/firmware/clients')
from robot_api import RobotApi

HOST = 'http://192.168.137.27'
b = RobotApi(HOST)

print('PWR OFF ->', b.radar_power(False).get('ok'))
time.sleep(3)
print('PWR ON  ->', b.radar_power(True).get('ok'))

for i in range(6):
    time.sleep(5)
    d = b.radar()
    print('t+%ds  link=%s rxBytes=%s rxLevel=%s frames59=%s frames5A=%s version=%s peek=%s'
          % ( (i+1)*5, d.get('link'), d.get('rxBytes'), d.get('rxLevel'),
               d.get('frames59'), d.get('frames5A'), d.get('version'), d.get('rxPeekHex')))
