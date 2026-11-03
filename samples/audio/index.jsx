import {useState} from 'react';
import {AppRegistry, View, Text, FocusScope, Sound} from '@ps4-react/core';
import font from '../assets/Inter-Regular.ttf';
import beep from './beep.wav';

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

function App() {
  const [volume, setVolume] = useState(1);
  const [status, setStatus] = useState('Press Play to hear the baked beep.');

  const play = (pan = 0) => {
    const started = Sound.play(beep, {volume, pan});
    setStatus(started ? `Played (volume ${volume.toFixed(2)}, pan ${pan})` : 'No audio output');
  };

  const changeVolume = () => {
    const next = volume > 0.6 ? 0.5 : volume > 0.3 ? 1 : 0.25;
    setVolume(next);
    Sound.setVolume(next);
    setStatus(`Master volume ${next.toFixed(2)}`);
  };

  return (
    <FocusScope wrap>
      <View style={{
        width: 1920, height: 1080, backgroundColor: '#101820',
        alignItems: 'center', justifyContent: 'center', gap: 30,
      }}>
        <Text style={{fontFamily: font, fontSize: 72, color: '#ffffff'}}>
          PS4 React Audio
        </Text>
        <Text style={{fontFamily: font, fontSize: 30, color: '#a0c4ff'}}>{status}</Text>
        <View style={{flexDirection: 'row', gap: 20}}>
          <Button label="Play beep" autoFocus onPress={() => play(0)} />
          <Button label="Play left" onPress={() => play(-0.8)} />
          <Button label="Play right" onPress={() => play(0.8)} />
        </View>
        <Button label={`Volume: ${volume.toFixed(2)}`} onPress={changeVolume} />
        <Text style={{fontFamily: font, fontSize: 23, color: '#5c6b78'}}>
          D-pad: choose    Cross: confirm
        </Text>
      </View>
    </FocusScope>
  );
}

AppRegistry.registerComponent('sample-audio', () => App);
