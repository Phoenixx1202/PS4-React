import {useEffect, useRef, useState} from 'react';
import {AppRegistry, View, Text, FocusScope, Video} from '@ps4-react/core';
import font from '../assets/Inter-Regular.ttf';
import tick from './tick.wav';

const SOURCE = '/data/pkg/sample.mp4';
const INITIAL_RECT = {x: 480, y: 220, width: 960, height: 540};

function Button({label, onPress, disabled, autoFocus}) {
  return (
    <View
      focusable
      autoFocus={autoFocus}
      onPress={disabled ? undefined : onPress}
      style={({focused}) => ({
        width: 220, height: 76, borderRadius: 12, borderWidth: 3,
        borderColor: disabled ? '#24323c' : focused ? '#a0c4ff' : '#314451',
        backgroundColor: disabled ? '#141d24' : focused ? '#294559' : '#182630',
        alignItems: 'center', justifyContent: 'center',
      })}>
      <Text style={{fontFamily: font, fontSize: 28, color: disabled ? '#5c6b78' : '#ffffff'}}>
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
  const [status, setStatus] = useState({state: 'idle', position: 0, error: ''});
  const [videoRect, setVideoRect] = useState(INITIAL_RECT);
  const trackRef = useRef(null);
  trackRef.current = track;

  const onVideoLayout = event => {
    const layout = event?.layout;
    if (!layout || layout.width <= 8 || layout.height <= 8) return;
    // The placeholder has a 4px border; draw inside it.
    const next = {x: layout.x + 4, y: layout.y + 4, width: layout.width - 8, height: layout.height - 8};
    setVideoRect(next);
    trackRef.current?.setRect(next.x, next.y, next.width, next.height);
  };

  useEffect(() => {
    const timer = setInterval(() => {
      const active = trackRef.current;
      if (!active) return;
      try {
        setStatus(active.status());
      } catch (error) {
        setStatus(previous => ({...previous, state: 'error', error: error.message}));
      }
    }, 250);
    return () => clearInterval(timer);
  }, []);

  const play = () => {
    try {
      const next = Video.play({path: SOURCE, ...videoRect});
      setTrack(next);
      setStatus({state: 'playing', position: 0, error: ''});
    } catch (error) {
      setStatus({state: 'error', position: 0, error: error.message});
    }
  };
  const pause = () => track && track.pause();
  const resume = () => track && track.resume();
  const seek = () => {
    if (!track) return;
    const duration = status.duration || 0;
    const target = Math.max(0, Math.min((status.position || 0) + 5, duration > 1 ? duration - 1 : 0));
    track.seek(target);
  };
  const stop = () => {
    if (track) track.stop();
    setTrack(null);
    setStatus({state: 'stopped', position: 0, error: ''});
  };

  return (
    <FocusScope wrap>
      <View style={{
        width: 1920, height: 1080, backgroundColor: '#101820',
        alignItems: 'center', paddingTop: 54, gap: 22,
      }}>
        <Text style={{fontFamily: font, fontSize: 62, color: '#ffffff'}}>
          PS4 React Video
        </Text>
        <Text style={{fontFamily: font, fontSize: 26, color: '#a0c4ff'}}>
          {`${status.state}  |  ${clock(status.position)} / ${clock(status.duration)}  |  ${status.width || 0}x${status.height || 0}  |  ${status.frames || 0} frames`}
        </Text>
        <View
          onLayout={onVideoLayout}
          style={{
            width: 968, height: 548, borderRadius: 8, borderWidth: 4,
            borderColor: '#314451', backgroundColor: '#000000',
          }}
        />
        {status.error && (
          <Text style={{fontFamily: font, fontSize: 23, color: '#ff8a8a'}}>{status.error}</Text>
        )}
        <View style={{flexDirection: 'row', gap: 16}}>
          <Button label="Play" autoFocus onPress={play} />
          <Button label="Pause" disabled={!track} onPress={pause} />
          <Button label="Resume" disabled={!track} onPress={resume} />
          <Button label="Seek +5s" disabled={!track} onPress={seek} />
          <Button label="Stop" disabled={!track} onPress={stop} />
        </View>
        <Text style={{fontFamily: font, fontSize: 22, color: '#5c6b78'}}>
          Put an H.264/AAC MP4 at {SOURCE} · {tick}
        </Text>
      </View>
    </FocusScope>
  );
}

AppRegistry.registerComponent('sample-video', () => App);
