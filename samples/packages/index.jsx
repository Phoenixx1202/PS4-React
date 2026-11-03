import {useState} from 'react';
import {AppRegistry, View, Text, FocusScope, Packages} from '@ps4-react/core';
import font from '../assets/Inter-Regular.ttf';

const PKG = '/data/pkg/PS4-React-counter.pkg';
const TITLE_ID = 'CUSA99060';

function Button({label, onPress, disabled, autoFocus}) {
  return (
    <View
      focusable
      autoFocus={autoFocus}
      onPress={disabled ? undefined : onPress}
      style={({focused}) => ({
        width: 560, height: 96, borderRadius: 12, borderWidth: 3,
        borderColor: disabled ? '#24323c' : focused ? '#a0c4ff' : '#314451',
        backgroundColor: disabled ? '#141d24' : focused ? '#294559' : '#182630',
        alignItems: 'center', justifyContent: 'center',
      })}>
      <Text style={{fontFamily: font, fontSize: 34, color: disabled ? '#5c6b78' : '#ffffff'}}>
        {label}
      </Text>
    </View>
  );
}

function formatProgress(snapshot) {
  const percent = snapshot.percent ?? 0;
  const parts = [`${snapshot.state} ${percent}%`];
  if (snapshot.titleId) parts.push(snapshot.titleId);
  if (snapshot.total) parts.push(`${snapshot.written}/${snapshot.total}`);
  else if (snapshot.contentId) parts.push(snapshot.contentId);
  return parts.join('  ');
}

function App() {
  const [busy, setBusy] = useState(false);
  const [status, setStatus] = useState('Ready.');
  const [detail, setDetail] = useState(`${PKG}  ->  ${TITLE_ID}`);
  const [error, setError] = useState('');

  const run = (label, start) => {
    if (busy) return;
    setBusy(true);
    setError('');
    setStatus(`${label} queued...`);
    setDetail('');
    let task;
    try {
      task = start();
    } catch (reason) {
      setBusy(false);
      setStatus(`${label} failed`);
      setError(reason.message || String(reason));
      return;
    }
    task.subscribe(snapshot => {
      setStatus(`${label}: ${snapshot.state}`);
      setDetail(formatProgress(snapshot));
    });
    task.done.then(result => {
      setBusy(false);
      setStatus(`${label} completed`);
      setDetail(formatProgress(result));
    }).catch(reason => {
      setBusy(false);
      setStatus(`${label} failed`);
      setError(reason.message || String(reason));
    });
  };

  const install = () => run('Install', () => Packages.install({path: PKG}));
  const uninstall = () => run('Uninstall', () => Packages.uninstall({titleId: TITLE_ID}));

  return (
    <FocusScope wrap>
      <View style={{
        width: 1920, height: 1080, backgroundColor: '#101820',
        alignItems: 'center', justifyContent: 'center', gap: 28,
      }}>
        <Text style={{fontFamily: font, fontSize: 72, color: '#ffffff'}}>
          PS4 React Packages
        </Text>
        <Text style={{fontFamily: font, fontSize: 30, color: '#a0c4ff'}}>
          {status}
        </Text>
        <Text style={{fontFamily: font, fontSize: 24, color: '#8fa6b8'}}>
          {detail || ' '}
        </Text>
        {error && (
          <Text style={{fontFamily: font, fontSize: 24, color: '#ff8a8a'}}>
            {error}
          </Text>
        )}
        <Button label="Install counter PKG" autoFocus disabled={busy} onPress={install} />
        <Button label="Uninstall counter title" disabled={busy} onPress={uninstall} />
        <Text style={{fontFamily: font, fontSize: 25, color: '#a0c4ff'}}>
          D-pad: choose    Cross: confirm
        </Text>
        <Text style={{fontFamily: font, fontSize: 22, color: '#5c6b78'}}>
          The counter PKG must be at {PKG}
        </Text>
      </View>
    </FocusScope>
  );
}

AppRegistry.registerComponent('sample-packages', () => App);
