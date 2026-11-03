import {useEffect, useState} from 'react';
import {AppRegistry, View, Text, FocusScope} from '@ps4-react/core';
import font from '../assets/Inter-Regular.ttf';

const COLUMNS = 16;
const ROWS = 9;

function App() {
  const [tick, setTick] = useState(0);

  useEffect(() => {
    const timer = setInterval(() => setTick(value => (value + 1) % 1000), 33);
    return () => clearInterval(timer);
  }, []);

  const cells = [];
  const palette = ['#294559', '#31455c', '#2d4a44', '#4a3949'];
  for (let row = 0; row < ROWS; ++row) {
    for (let column = 0; column < COLUMNS; ++column) {
      const index = row * COLUMNS + column;
      const active = (index + tick) % 37 < 4;
      cells.push(
        <View key={index} style={{
          width: 96, height: 72, borderRadius: 8,
          backgroundColor: active ? '#a0c4ff' : palette[(index + Math.floor(tick / 10)) % palette.length],
        }} />,
      );
    }
  }

  return (
    <FocusScope wrap>
      <View style={{
        width: 1920, height: 1080, backgroundColor: '#101820',
        alignItems: 'center', justifyContent: 'center', gap: 18,
      }}>
        <Text style={{fontFamily: font, fontSize: 64, color: '#ffffff'}}>
          PS4 React GPU
        </Text>
        <Text style={{fontFamily: font, fontSize: 28, color: '#a0c4ff'}}>
          {`Frame ${tick}  |  Piglet presentation with software fallback`}
        </Text>
        <View style={{flexDirection: 'row', flexWrap: 'wrap', width: COLUMNS * 104, gap: 8}}>
          {cells}
        </View>
        <Text style={{fontFamily: font, fontSize: 22, color: '#5c6b78'}}>
          The log says gpu presentation=yes when Piglet is active.
        </Text>
      </View>
    </FocusScope>
  );
}

AppRegistry.registerComponent('sample-gpu', () => App);
