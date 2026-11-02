import {useState} from 'react';
import {AppRegistry, View, Text, FocusScope} from '@ps4-react/core';
import font from '../assets/Inter-Regular.ttf';

function Button({label, onPress, autoFocus}) {
  return (
    <View
      focusable
      autoFocus={autoFocus}
      onPress={onPress}
      style={({focused}) => ({
        width: 360, height: 96, borderRadius: 12, borderWidth: 3,
        borderColor: focused ? '#a0c4ff' : '#314451',
        backgroundColor: focused ? '#294559' : '#182630',
        alignItems: 'center', justifyContent: 'center',
      })}>
      <Text style={{fontFamily: font, fontSize: 36, color: '#ffffff'}}>{label}</Text>
    </View>
  );
}

function App() {
  const [count, setCount] = useState(0);
  return (
    <FocusScope wrap>
      <View style={{
        width: 1920, height: 1080, backgroundColor: '#101820',
        alignItems: 'center', justifyContent: 'center', gap: 36,
      }}>
        <Text style={{fontFamily: font, fontSize: 80, color: '#ffffff'}}>
          PS4 React Counter
        </Text>
        <Text style={{fontFamily: font, fontSize: 48, color: '#a0c4ff'}}>
          Count: {count}
        </Text>
        <View style={{flexDirection: 'row', gap: 24}}>
          <Button label="Add 1" autoFocus onPress={() => setCount(value => value + 1)} />
          <Button label="Add 10" onPress={() => setCount(value => value + 10)} />
          <Button label="Reset" onPress={() => setCount(0)} />
        </View>
        <Text style={{fontFamily: font, fontSize: 27, color: '#a0c4ff'}}>
          D-pad: choose    Cross: confirm
        </Text>
      </View>
    </FocusScope>
  );
}

AppRegistry.registerComponent('sample-counter', () => App);
