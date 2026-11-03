import {useState} from 'react';
import {AppRegistry, View, Text, FocusScope, Archives, FileSystem} from '@ps4-react/core';
import font from '../assets/Inter-Regular.ttf';

const ZIP = '/download0/sample.zip';
const OUT = '/download0/extracted';

function Button({label, onPress, autoFocus}) {
  return (
    <View
      focusable
      autoFocus={autoFocus}
      onPress={onPress}
      style={({focused}) => ({
        width: 480, height: 92, borderRadius: 12, borderWidth: 3,
        borderColor: focused ? '#a0c4ff' : '#314451',
        backgroundColor: focused ? '#294559' : '#182630',
        alignItems: 'center', justifyContent: 'center',
      })}>
      <Text style={{fontFamily: font, fontSize: 32, color: '#ffffff'}}>{label}</Text>
    </View>
  );
}

function App() {
  const [status, setStatus] = useState('Create a ZIP, then extract it.');
  const [detail, setDetail] = useState('');
  const [error, setError] = useState('');

  const watch = (label, task) => {
    task.subscribe(snapshot => {
      setStatus(`${label}: ${snapshot.state}`);
      setDetail(`${snapshot.written ?? 0} bytes  |  ${snapshot.entries ?? 0} entries`);
    });
    task.done.then(result => {
      setStatus(`${label} completed.`);
      setDetail(`${result.written ?? 0} bytes  |  ${result.entries ?? 0} entries`);
    }).catch(reason => {
      setStatus(`${label} failed.`);
      setError(reason.message || String(reason));
    });
  };

  const create = () => {
    setError('');
    watch('Create', Archives.create({
      sources: ['/app0/ca-bundle.crt'],
      destination: ZIP,
      format: 'zip',
      overwrite: true,
    }));
  };

  const extract = () => {
    setError('');
    watch('Extract', Archives.extract({
      sources: [ZIP],
      destination: OUT,
      maxBytes: 10 * 1024 * 1024,
    }));
  };

  const inspect = () => {
    setError('');
    try {
      const info = Archives.inspect([ZIP]);
      setStatus(`Archive: ${info.kind || 'unknown'}`);
      setDetail(info.refusal || 'Headers accepted.');
    } catch (reason) {
      setStatus('Inspect failed.');
      setError(reason.message || String(reason));
    }
  };

  const verify = () => {
    setError('');
    try {
      const file = `${OUT}/ca-bundle.crt`;
      setStatus(FileSystem.exists(file) ? 'Extracted ca-bundle.crt found.' : 'ca-bundle.crt is missing.');
      setDetail(file);
    } catch (reason) {
      setStatus('Verify failed.');
      setError(reason.message || String(reason));
    }
  };

  return (
    <FocusScope wrap>
      <View style={{
        width: 1920, height: 1080, backgroundColor: '#101820',
        alignItems: 'center', justifyContent: 'center', gap: 24,
      }}>
        <Text style={{fontFamily: font, fontSize: 72, color: '#ffffff'}}>
          PS4 React Archive
        </Text>
        <Text style={{fontFamily: font, fontSize: 30, color: '#a0c4ff'}}>{status}</Text>
        <Text style={{fontFamily: font, fontSize: 24, color: '#8fa6b8'}}>{detail || ' '}</Text>
        {error && (
          <Text style={{fontFamily: font, fontSize: 24, color: '#ff8a8a'}}>{error}</Text>
        )}
        <View style={{flexDirection: 'row', gap: 20}}>
          <Button label="Create ZIP" autoFocus onPress={create} />
          <Button label="Extract ZIP" onPress={extract} />
        </View>
        <View style={{flexDirection: 'row', gap: 20}}>
          <Button label="Inspect" onPress={inspect} />
          <Button label="Verify" onPress={verify} />
        </View>
        <Text style={{fontFamily: font, fontSize: 23, color: '#5c6b78'}}>
          D-pad: choose    Cross: confirm
        </Text>
      </View>
    </FocusScope>
  );
}

AppRegistry.registerComponent('sample-archive', () => App);
