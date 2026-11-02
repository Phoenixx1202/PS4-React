import {useEffect, useState} from 'react';
import {AppRegistry, View, Text, FocusScope, FileSystem, Keyboard, SaveData} from '@ps4-react/core';
import font from '../assets/Inter-Regular.ttf';

const DIR = 'settings';
const FILE = 'settings.json';

function Button({label, onPress, autoFocus}) {
  return (
    <View
      focusable
      autoFocus={autoFocus}
      onPress={onPress}
      style={({focused}) => ({
        width: 420, height: 96, borderRadius: 12, borderWidth: 3,
        borderColor: focused ? '#a0c4ff' : '#314451',
        backgroundColor: focused ? '#294559' : '#182630',
        alignItems: 'center', justifyContent: 'center',
      })}>
      <Text style={{fontFamily: font, fontSize: 34, color: '#ffffff'}}>{label}</Text>
    </View>
  );
}

function App() {
  const [state, setState] = useState({name: 'Player', path: '', status: 'Mounting save data...', error: ''});

  useEffect(() => {
    let mounted = true;
    try {
      const save = SaveData.mount({dirName: DIR, blocks: 96, title: 'PS4 React Settings'});
      const file = `${save.path}/${FILE}`;
      let name = 'Player';
      if (FileSystem.exists(file)) {
        try { name = JSON.parse(FileSystem.readFile(file)).name || name; } catch {}
      }
      if (mounted) setState({name, path: save.path, status: 'Saved locally', error: ''});
      return () => {
        mounted = false;
        try { SaveData.unmount(DIR); } catch {}
      };
    } catch (error) {
      if (mounted) setState(previous => ({...previous, status: 'Save data unavailable', error: error.message}));
    }
    return () => { mounted = false; };
  }, []);

  const edit = async () => {
    try {
      const task = Keyboard.prompt({
        title: 'Player name', placeholder: 'Type a name', value: state.name, maxLength: 24,
      });
      const result = await task.done;
      const name = result.text || state.name;
      if (state.path) {
        FileSystem.writeFile(`${state.path}/${FILE}`, `${JSON.stringify({name})}\n`, {sync: true});
      }
      setState(previous => ({...previous, name, status: 'Saved locally', error: ''}));
    } catch (error) {
      setState(previous => ({...previous, status: 'Input cancelled', error: error.message}));
    }
  };

  return (
    <FocusScope wrap>
      <View style={{
        width: 1920, height: 1080, backgroundColor: '#101820',
        alignItems: 'center', justifyContent: 'center', gap: 32,
      }}>
        <Text style={{fontFamily: font, fontSize: 72, color: '#ffffff'}}>
          PS4 React Settings
        </Text>
        <Text style={{fontFamily: font, fontSize: 46, color: '#a0c4ff'}}>
          {state.name}
        </Text>
        <Text style={{fontFamily: font, fontSize: 30, color: '#a0c4ff'}}>
          {state.path || 'No save path'} — {state.status}
        </Text>
        {state.error && (
          <Text style={{fontFamily: font, fontSize: 24, color: '#ff8a8a'}}>
            {state.error}
          </Text>
        )}
        <Button label="Edit name" autoFocus onPress={edit} />
        <Text style={{fontFamily: font, fontSize: 25, color: '#a0c4ff'}}>
          D-pad: choose    Cross: confirm
        </Text>
      </View>
    </FocusScope>
  );
}

AppRegistry.registerComponent('sample-settings', () => App);
