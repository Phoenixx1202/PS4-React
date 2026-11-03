import {useState} from 'react';
import {AppRegistry, View, Text, FocusScope, Dialog, Payloads} from '@ps4-react/core';
import font from '../assets/Inter-Regular.ttf';

const PAYLOAD = '/data/pkg/payload.elf';

function Button({label, onPress, autoFocus}) {
  return (
    <View
      focusable
      autoFocus={autoFocus}
      onPress={onPress}
      style={({focused}) => ({
        width: 560, height: 96, borderRadius: 12, borderWidth: 3,
        borderColor: focused ? '#a0c4ff' : '#314451',
        backgroundColor: focused ? '#294559' : '#182630',
        alignItems: 'center', justifyContent: 'center',
      })}>
      <Text style={{fontFamily: font, fontSize: 34, color: '#ffffff'}}>{label}</Text>
    </View>
  );
}

function App() {
  const [status, setStatus] = useState('Ready.');
  const [detail, setDetail] = useState(`Put an ELF payload at ${PAYLOAD}`);
  const [error, setError] = useState('');

  const check = () => {
    setError('');
    const info = Payloads.diagnose();
    setStatus(info.available ? 'BinLoader is reachable.' : 'BinLoader is not available.');
    setDetail(`loopback: ${info.loopback ? 'ok' : `error ${info.loopbackError}`}  |  `
      + `${info.ip || '?'}:${info.port}: ${info.lan ? 'ok' : `error ${info.lanError}`}  |  `
      + `FTP 2121: ${info.ftpLoopback || info.ftpTcp ? 'ok' : `error ${info.ftpLoopbackError}`}`);
  };

  const send = async () => {
    setError('');
    const confirmed = await Dialog.confirm(
      'Send payload?',
      `${PAYLOAD}\n\nGoldHEN will execute this file. Only send payloads you trust.`);
    if (!confirmed) {
      setStatus('Send cancelled.');
      return;
    }
    try {
      const task = Payloads.send({path: PAYLOAD});
      task.subscribe(snapshot => {
        setStatus(`Sending: ${snapshot.state}`);
        setDetail(`${snapshot.written ?? 0} / ${snapshot.total ?? 0} bytes`);
      });
      const result = await task.done;
      setStatus('Payload sent.');
      setDetail(`${result.total ?? 0} bytes`);
    } catch (reason) {
      setStatus('Send failed.');
      setError(reason.message || String(reason));
    }
  };

  return (
    <FocusScope wrap>
      <View style={{
        width: 1920, height: 1080, backgroundColor: '#101820',
        alignItems: 'center', justifyContent: 'center', gap: 30,
      }}>
        <Text style={{fontFamily: font, fontSize: 72, color: '#ffffff'}}>
          PS4 React Payloads
        </Text>
        <Text style={{fontFamily: font, fontSize: 32, color: '#a0c4ff'}}>
          {status}
        </Text>
        <Text style={{fontFamily: font, fontSize: 24, color: '#8fa6b8'}}>
          {detail}
        </Text>
        {error && (
          <Text style={{fontFamily: font, fontSize: 24, color: '#ff8a8a'}}>
            {error}
          </Text>
        )}
        <Button label="Check BinLoader" autoFocus onPress={check} />
        <Button label="Send payload" onPress={send} />
        <Text style={{fontFamily: font, fontSize: 24, color: '#5c6b78'}}>
          Only send ELF files you trust; GoldHEN executes them.
        </Text>
      </View>
    </FocusScope>
  );
}

AppRegistry.registerComponent('sample-payloads', () => App);
