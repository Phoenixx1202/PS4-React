import {useEffect, useState} from 'react';
import {AppRegistry, View, Text, FocusScope} from '@ps4-react/core';
import {Http} from '@ps4-react/core';
import font from '../assets/Inter-Regular.ttf';

function App() {
  const [state, setState] = useState({loading: true, status: 0, bytes: 0, error: ''});

  const load = () => {
    setState({loading: true, status: 0, bytes: 0, error: ''});
    Http.request('https://example.com/')
      .then(async response => {
        const body = await response.text();
        setState({loading: false, status: response.status, bytes: body.length, error: ''});
      })
      .catch(error => setState({loading: false, status: 0, bytes: 0, error: error.message}));
  };

  useEffect(load, []);

  return (
    <FocusScope wrap>
      <View style={{
        width: 1920, height: 1080, backgroundColor: '#101820',
        alignItems: 'center', justifyContent: 'center', gap: 32,
      }}>
        <Text style={{fontFamily: font, fontSize: 80, color: '#ffffff'}}>
          PS4 React Network
        </Text>
        {state.loading && (
          <Text style={{fontFamily: font, fontSize: 40, color: '#a0c4ff'}}>
            Requesting https://example.com/...
          </Text>
        )}
        {!state.loading && !state.error && (
          <Text style={{fontFamily: font, fontSize: 40, color: '#a0c4ff'}}>
            HTTP {state.status} — {state.bytes} bytes
          </Text>
        )}
        {state.error && (
          <Text style={{fontFamily: font, fontSize: 32, color: '#ff8a8a'}}>
            {state.error}
          </Text>
        )}
        <View
          focusable
          autoFocus
          onPress={load}
          style={({focused}) => ({
            width: 320, height: 88, borderRadius: 12, borderWidth: 3,
            borderColor: focused ? '#a0c4ff' : '#314451',
            backgroundColor: focused ? '#294559' : '#182630',
            alignItems: 'center', justifyContent: 'center',
          })}>
          <Text style={{fontFamily: font, fontSize: 36, color: '#ffffff'}}>Request again</Text>
        </View>
        <Text style={{fontFamily: font, fontSize: 27, color: '#a0c4ff'}}>
          D-pad: choose    Cross: confirm
        </Text>
      </View>
    </FocusScope>
  );
}

AppRegistry.registerComponent('sample-network', () => App);
