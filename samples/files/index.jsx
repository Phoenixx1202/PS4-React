import {useEffect, useState} from 'react';
import {AppRegistry, View, Text, FocusScope, FileSystem} from '@ps4-react/core';
import font from '../assets/Inter-Regular.ttf';

const FILE = '/download0/sample.txt';

function App() {
  const [state, setState] = useState({text: '', entries: [], error: ''});

  const run = () => {
    try {
      FileSystem.writeFile(FILE, 'Hello from PS4 React\n');
      setState({
        text: FileSystem.readFile(FILE),
        entries: FileSystem.readDir('/download0').map(entry => entry.name),
        error: '',
      });
    } catch (error) {
      setState({text: '', entries: [], error: error.message});
    }
  };

  useEffect(run, []);

  return (
    <FocusScope wrap>
      <View style={{
        width: 1920, height: 1080, backgroundColor: '#101820',
        alignItems: 'center', justifyContent: 'center', gap: 28,
      }}>
        <Text style={{fontFamily: font, fontSize: 72, color: '#ffffff'}}>
          PS4 React Files
        </Text>
        <Text style={{fontFamily: font, fontSize: 32, color: '#a0c4ff'}}>
          /download0 → /data/PS4React
        </Text>
        <Text style={{fontFamily: font, fontSize: 36, color: '#ffffff'}}>
          {state.text || 'No content'}
        </Text>
        <Text style={{fontFamily: font, fontSize: 27, color: '#a0c4ff'}}>
          Files: {state.entries.join(', ') || '(empty)'}
        </Text>
        {state.error && (
          <Text style={{fontFamily: font, fontSize: 27, color: '#ff8a8a'}}>
            {state.error}
          </Text>
        )}
        <View
          focusable
          autoFocus
          onPress={run}
          style={({focused}) => ({
            width: 320, height: 88, borderRadius: 12, borderWidth: 3,
            borderColor: focused ? '#a0c4ff' : '#314451',
            backgroundColor: focused ? '#294559' : '#182630',
            alignItems: 'center', justifyContent: 'center',
          })}>
          <Text style={{fontFamily: font, fontSize: 36, color: '#ffffff'}}>Read again</Text>
        </View>
      </View>
    </FocusScope>
  );
}

AppRegistry.registerComponent('sample-files', () => App);
