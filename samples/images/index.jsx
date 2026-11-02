import {useState} from 'react';
import {AppRegistry, View, Text, Image, FocusScope} from '@ps4-react/core';
import font from '../assets/Inter-Regular.ttf';

const SOURCE = 'https://raw.githubusercontent.com/github/explore/main/topics/react/react.png';

function App() {
  const [state, setState] = useState('loading');

  return (
    <FocusScope wrap>
      <View style={{
        width: 1920, height: 1080, backgroundColor: '#101820',
        alignItems: 'center', justifyContent: 'center', gap: 28,
      }}>
        <Text style={{fontFamily: font, fontSize: 72, color: '#ffffff'}}>
          PS4 React Images
        </Text>
        <Image
          source={{uri: SOURCE}}
          resizeMode="contain"
          style={{width: 640, height: 360, borderRadius: 16, backgroundColor: '#182630'}}
          onLoad={() => setState('ready')}
          onError={event => setState(event.nativeEvent.error)}
        />
        <Text style={{fontFamily: font, fontSize: 32, color: state === 'ready' ? '#a0c4ff' : '#ffb86b'}}>
          {state === 'loading' ? 'Loading remote image...' : state}
        </Text>
      </View>
    </FocusScope>
  );
}

AppRegistry.registerComponent('sample-images', () => App);
