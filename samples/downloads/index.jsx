import {useState} from 'react';
import {AppRegistry, View, Text, FocusScope, Downloads, FileSystem} from '@ps4-react/core';
import font from '../assets/Inter-Regular.ttf';

const URL = 'https://proof.ovh.net/files/10Mb.dat';
const DEST = '/download0/10Mb.dat';

function Button({label, onPress, disabled, autoFocus}) {
  return (
    <View
      focusable
      autoFocus={autoFocus}
      onPress={disabled ? undefined : onPress}
      style={({focused}) => ({
        width: 380, height: 88, borderRadius: 12, borderWidth: 3,
        borderColor: disabled ? '#24323c' : focused ? '#a0c4ff' : '#314451',
        backgroundColor: disabled ? '#141d24' : focused ? '#294559' : '#182630',
        alignItems: 'center', justifyContent: 'center',
      })}>
      <Text style={{fontFamily: font, fontSize: 32, color: disabled ? '#5c6b78' : '#ffffff'}}>
        {label}
      </Text>
    </View>
  );
}

function formatBytes(value) {
  if (value == null || value < 0) return '?';
  if (value >= 1024 * 1024) return `${(value / (1024 * 1024)).toFixed(1)} MB`;
  if (value >= 1024) return `${(value / 1024).toFixed(0)} KB`;
  return `${value} B`;
}

function App() {
  const [task, setTask] = useState(null);
  const [status, setStatus] = useState('Ready.');
  const [detail, setDetail] = useState(URL);
  const [error, setError] = useState('');

  const start = () => {
    setError('');
    setStatus('Queued.');
    const next = Downloads.enqueue({url: URL, destination: DEST});
    setTask(next);
    next.subscribe(snapshot => {
      setStatus(`Download: ${snapshot.state}`);
      setDetail(`${formatBytes(snapshot.written)} / ${formatBytes(snapshot.total)}`
        + `  |  ${formatBytes(snapshot.bytesPerSecond)}/s  |  ${snapshot.connections ?? 0} connections`);
    });
    next.done.then(result => {
      setStatus('Download completed.');
      try {
        const file = FileSystem.stat(DEST);
        setDetail(`${DEST}  |  ${formatBytes(file?.size ?? result.written)}`);
      } catch {
        setDetail(`${DEST}  |  ${formatBytes(result.written)}`);
      }
    }).catch(reason => {
      setStatus(reason.name === 'AbortError' ? 'Download cancelled.' : 'Download failed.');
      if (reason.name !== 'AbortError') setError(reason.message || String(reason));
    });
  };

  const cancel = () => {
    if (task) task.cancel();
  };

  return (
    <FocusScope wrap>
      <View style={{
        width: 1920, height: 1080, backgroundColor: '#101820',
        alignItems: 'center', justifyContent: 'center', gap: 28,
      }}>
        <Text style={{fontFamily: font, fontSize: 72, color: '#ffffff'}}>
          PS4 React Downloads
        </Text>
        <Text style={{fontFamily: font, fontSize: 32, color: '#a0c4ff'}}>{status}</Text>
        <Text style={{fontFamily: font, fontSize: 24, color: '#8fa6b8'}}>{detail}</Text>
        {error && (
          <Text style={{fontFamily: font, fontSize: 24, color: '#ff8a8a'}}>{error}</Text>
        )}
        <View style={{flexDirection: 'row', gap: 20}}>
          <Button label="Start" autoFocus onPress={start} />
          <Button label="Cancel" disabled={!task} onPress={cancel} />
          <Button label="Retry" onPress={start} />
        </View>
        <Text style={{fontFamily: font, fontSize: 23, color: '#5c6b78'}}>
          {`10 MB test file → ${DEST}`}
        </Text>
      </View>
    </FocusScope>
  );
}

AppRegistry.registerComponent('sample-downloads', () => App);
