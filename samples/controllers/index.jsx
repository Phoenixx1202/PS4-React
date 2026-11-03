import {AppRegistry, View, Text, FocusScope, useGamepad} from '@ps4-react/core';
import font from '../assets/Inter-Regular.ttf';

function stick(value) {
  return value >= 0 ? `+${value.toFixed(2)}` : value.toFixed(2);
}

function Player({index}) {
  const pad = useGamepad(50, index);
  return (
    <View style={{
      width: 420, height: 250, borderRadius: 14, borderWidth: 3,
      borderColor: pad.connected ? '#4caf7d' : '#314451',
      backgroundColor: '#182630', padding: 18, gap: 6,
    }}>
      <Text style={{fontFamily: font, fontSize: 30, color: pad.connected ? '#7ee2a8' : '#5c6b78'}}>
        {`Player ${index + 1}  ${pad.connected ? 'connected' : 'offline'}`}
      </Text>
      <Text style={{fontFamily: font, fontSize: 21, color: '#c8d6e5'}}>
        {`L ${stick(pad.leftX)} / ${stick(pad.leftY)}`}
      </Text>
      <Text style={{fontFamily: font, fontSize: 21, color: '#c8d6e5'}}>
        {`R ${stick(pad.rightX)} / ${stick(pad.rightY)}`}
      </Text>
      <Text style={{fontFamily: font, fontSize: 21, color: '#c8d6e5'}}>
        {`L2 ${pad.l2.toFixed(2)}  R2 ${pad.r2.toFixed(2)}`}
      </Text>
      <Text style={{fontFamily: font, fontSize: 19, color: '#8fa6b8'}}>
        {pad.buttons.length ? pad.buttons.join(' + ') : 'no buttons'}
      </Text>
    </View>
  );
}

function App() {
  return (
    <FocusScope wrap>
      <View style={{
        width: 1920, height: 1080, backgroundColor: '#101820',
        alignItems: 'center', justifyContent: 'center', gap: 24,
      }}>
        <Text style={{fontFamily: font, fontSize: 70, color: '#ffffff'}}>
          PS4 React Controllers
        </Text>
        <View style={{flexDirection: 'row', gap: 20}}>
          <Player index={0} />
          <Player index={1} />
        </View>
        <View style={{flexDirection: 'row', gap: 20}}>
          <Player index={2} />
          <Player index={3} />
        </View>
        <Text style={{fontFamily: font, fontSize: 23, color: '#5c6b78'}}>
          Connect a pad per logged-in user; any pad navigates the UI.
        </Text>
      </View>
    </FocusScope>
  );
}

AppRegistry.registerComponent('sample-controllers', () => App);
