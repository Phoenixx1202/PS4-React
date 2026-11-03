import {useEffect, useState} from 'react';
import {AppRegistry, View, Text, FocusScope, Music} from '@ps4-react/core';
import font from '../assets/Inter-Regular.ttf';
import tick from './tick.wav';

const SONG = '/data/pkg/music.mp3';

function Button({label, onPress, disabled, autoFocus}) {
  return (
    <View
      focusable
      autoFocus={autoFocus}
      onPress={disabled ? undefined : onPress}
      style={({focused}) => ({
        width: 340, height: 88, borderRadius: 12, borderWidth: 3,
        borderColor: disabled ? '#24323c' : focused ? '#a0c4ff' : '#314451',
        backgroundColor: disabled ? '#141d24' : focused ? '#294559' : '#182630',
        alignItems: 'center', justifyContent: 'center',
      })}>
      <Text style={{fontFamily: font, fontSize: 31, color: disabled ? '#5c6b78' : '#ffffff'}}>
        {label}
      </Text>
    </View>
  );
}

function clock(value) {
  const seconds = Math.max(0, Math.floor(value || 0));
  return `${Math.floor(seconds / 60)}:${String(seconds % 60).padStart(2, '0')}`;
}

function App() {
  const [track, setTrack] = useState(null);
  const [status, setStatus] = useState({state: 'idle', position: 0, duration: 0, error: ''});
  const [volume, setVolume] = useState(1);

  useEffect(() => {
    if (!track) return undefined;
    const timer = setInterval(() => {
      try {
        setStatus(track.status());
      } catch (error) {
        setStatus(previous => ({...previous, state: 'error', error: error.message}));
      }
    }, 250);
    return () => clearInterval(timer);
  }, [track]);

  const play = () => {
    try {
      const next = Music.play({path: SONG, volume});
      setTrack(next);
      setStatus({state: 'playing', position: 0, duration: 0, error: ''});
    } catch (error) {
      setStatus({state: 'error', position: 0, duration: 0, error: error.message});
    }
  };

  const pause = () => { if (track) track.pause(); };
  const resume = () => { if (track) track.resume(); };
  const stop = () => { if (track) track.stop(); };

  const changeVolume = () => {
    const next = volume > 0.6 ? 0.4 : volume > 0.3 ? 1 : 0.2;
    setVolume(next);
    Music.setVolume(next);
  };

  return (
    <FocusScope wrap>
      <View style={{
        width: 1920, height: 1080, backgroundColor: '#101820',
        alignItems: 'center', justifyContent: 'center', gap: 26,
      }}>
        <Text style={{fontFamily: font, fontSize: 72, color: '#ffffff'}}>
          PS4 React Music
        </Text>
        <Text style={{fontFamily: font, fontSize: 30, color: '#a0c4ff'}}>
          {`${status.state}  |  ${clock(status.position)} / ${clock(status.duration)}`}
        </Text>
        <Text style={{fontFamily: font, fontSize: 24, color: '#8fa6b8'}}>{SONG}</Text>
        {status.error && (
          <Text style={{fontFamily: font, fontSize: 24, color: '#ff8a8a'}}>{status.error}</Text>
        )}
        <View style={{flexDirection: 'row', gap: 18}}>
          <Button label="Play" autoFocus onPress={play} />
          <Button label="Pause" disabled={!track} onPress={pause} />
          <Button label="Resume" disabled={!track} onPress={resume} />
          <Button label="Stop" disabled={!track} onPress={stop} />
        </View>
        <Button label={`Volume: ${volume.toFixed(2)}`} onPress={changeVolume} />
        <Text style={{fontFamily: font, fontSize: 22, color: '#5c6b78'}}>
          Put an MP3 at /data/pkg/music.mp3. Imported tick: {tick}
        </Text>
      </View>
    </FocusScope>
  );
}

AppRegistry.registerComponent('sample-music', () => App);
