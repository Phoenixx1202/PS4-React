import {useEffect, useState} from 'react';
import {AppRegistry, View, Text, FocusScope, Dialog, AppState} from '@ps4-react/core';
import font from '../assets/Inter-Regular.ttf';

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
  const [status, setStatus] = useState('Choose a dialog.');
  const [app, setApp] = useState({state: 'active', event: 'none'});

  useEffect(() => {
    try {
      const subscription = AppState.addEventListener('change', info => {
        setApp({state: info.state, event: info.event});
      });
      return () => subscription.remove();
    } catch {
      return undefined;
    }
  }, []);

  const alert = async () => {
    try {
      await Dialog.alert({
        title: 'PS4 React',
        message: 'This is a system message dialog. Cross closes it.',
      });
      setStatus('Alert closed.');
    } catch (error) {
      setStatus(`Alert failed: ${error.message}`);
    }
  };

  const confirm = async () => {
    try {
      const yes = await Dialog.confirm({
        title: 'Confirm',
        message: 'Do you want to continue?',
      });
      setStatus(yes ? 'Confirmed.' : 'Cancelled.');
    } catch (error) {
      setStatus(`Confirm failed: ${error.message}`);
    }
  };

  const progress = () => {
    try {
      const dialog = Dialog.progress({title: 'Progress', message: 'Starting...'});
      let value = 0;
      const timer = setInterval(() => {
        value = Math.min(100, value + 5);
        dialog.set(value, `Step ${value}%`);
        if (value >= 100) {
          clearInterval(timer);
          setTimeout(() => dialog.close(), 300);
        }
      }, 100);
      dialog.done.then(() => setStatus('Progress closed.')).catch(error => {
        setStatus(`Progress failed: ${error.message}`);
      });
    } catch (error) {
      setStatus(`Progress failed: ${error.message}`);
    }
  };

  return (
    <FocusScope wrap>
      <View style={{
        width: 1920, height: 1080, backgroundColor: '#101820',
        alignItems: 'center', justifyContent: 'center', gap: 32,
      }}>
        <Text style={{fontFamily: font, fontSize: 72, color: '#ffffff'}}>
          System dialogs
        </Text>
        <Text style={{fontFamily: font, fontSize: 30, color: '#c8d6e5'}}>
          {status}
        </Text>
        <Text style={{fontFamily: font, fontSize: 24, color: '#8fa6b8'}}>
          {`AppState: ${app.state} / ${app.event}`}
        </Text>
        <View style={{flexDirection: 'row', gap: 24}}>
          <Button label="Alert" onPress={alert} autoFocus />
          <Button label="Confirm" onPress={confirm} />
          <Button label="Progress" onPress={progress} />
        </View>
      </View>
    </FocusScope>
  );
}

AppRegistry.registerComponent('Dialogs', () => App);
