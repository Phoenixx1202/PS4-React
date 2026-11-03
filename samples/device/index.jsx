import {useState} from 'react';
import {AppRegistry, View, Text, FocusScope, DeviceInfo, Users, Notifications} from '@ps4-react/core';
import font from '../assets/Inter-Regular.ttf';

function Button({label, onPress, autoFocus}) {
  return (
    <View
      focusable
      autoFocus={autoFocus}
      onPress={onPress}
      style={({focused}) => ({
        width: 420, height: 88, borderRadius: 12, borderWidth: 3,
        borderColor: focused ? '#a0c4ff' : '#314451',
        backgroundColor: focused ? '#294559' : '#182630',
        alignItems: 'center', justifyContent: 'center',
      })}>
      <Text style={{fontFamily: font, fontSize: 32, color: '#ffffff'}}>{label}</Text>
    </View>
  );
}

function snapshot() {
  return {
    device: DeviceInfo.get(),
    foreground: Users.getForeground(),
    users: Users.getLoggedIn(),
  };
}

function App() {
  const [state, setState] = useState(snapshot);
  const {device, foreground, users} = state;
  const mb = value => value >= 0 ? `${(value / (1024 * 1024)).toFixed(0)} MB` : '?';

  return (
    <FocusScope wrap>
      <View style={{
        width: 1920, height: 1080, backgroundColor: '#101820',
        alignItems: 'center', justifyContent: 'center', gap: 22,
      }}>
        <Text style={{fontFamily: font, fontSize: 70, color: '#ffffff'}}>
          PS4 React Device
        </Text>
        <View style={{alignItems: 'center', gap: 4}}>
          <Text style={{fontFamily: font, fontSize: 30, color: '#a0c4ff'}}>
            {`${device.model || 'PS4'}  |  firmware ${device.firmware || '?'}  |  ${device.language || '?'}`}
          </Text>
          <Text style={{fontFamily: font, fontSize: 26, color: '#c8d6e5'}}>
            {`CPU ${device.cpuFrequency > 0 ? `${(device.cpuFrequency / 1000000).toFixed(0)} MHz` : '?'}  |  free ${mb(device.freeMemory)}`}
          </Text>
          <Text style={{fontFamily: font, fontSize: 26, color: '#c8d6e5'}}>
            {`CPU temp ${device.cpuTemperature >= 0 ? `${device.cpuTemperature}C` : '?'}  |  SoC temp ${device.socTemperature >= 0 ? `${device.socTemperature}C` : '?'}`}
          </Text>
          <Text style={{fontFamily: font, fontSize: 26, color: '#c8d6e5'}}>
            {`Foreground user: ${foreground ? foreground.name : 'none'}  |  logged in: ${users.length}`}
          </Text>
          {users.map(user => (
            <Text key={user.id} style={{fontFamily: font, fontSize: 23, color: '#8fa6b8'}}>
              {`${user.name} (0x${(user.id >>> 0).toString(16)})`}
            </Text>
          ))}
        </View>
        <View style={{flexDirection: 'row', gap: 20}}>
          <Button label="Refresh" autoFocus onPress={() => setState(snapshot())} />
          <Button label="Notify" onPress={() => Notifications.show('PS4 React', 'Hello from React')} />
        </View>
        <Text style={{fontFamily: font, fontSize: 23, color: '#5c6b78'}}>
          D-pad: choose    Cross: confirm
        </Text>
      </View>
    </FocusScope>
  );
}

AppRegistry.registerComponent('sample-device', () => App);
