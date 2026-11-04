import {useCallback, useEffect, useRef, useState} from 'react';
import {AppRegistry, View, Text, Image, FocusScope, Music} from '@ps4-react/core';
import font from '../assets/Inter-Regular.ttf';
import tick from './tick.wav';

const HTTP_SOURCE = 'http://192.168.0.8:8010/music.mp3';
const HTTP_OGG = 'http://192.168.0.8:8010/music.ogg';
const TRACKS = [
  {label: 'Local MP3', path: '/data/pkg/music.mp3'},
  {label: 'HTTP MP3', path: HTTP_SOURCE},
  {label: 'Local OGG', path: '/data/pkg/music.ogg'},
  {label: 'HTTP OGG', path: HTTP_OGG},
];

function Button({label, onPress, disabled, autoFocus}) {
  return (
    <View
      focusable
      autoFocus={autoFocus}
      onPress={disabled ? undefined : onPress}
      style={({focused}) => ({
        width: 250, height: 76, borderRadius: 12, borderWidth: 3,
        borderColor: disabled ? '#24323c' : focused ? '#a0c4ff' : '#314451',
        backgroundColor: disabled ? '#141d24' : focused ? '#294559' : '#182630',
        alignItems: 'center', justifyContent: 'center',
      })}>
      <Text style={{fontFamily: font, fontSize: 27, color: disabled ? '#5c6b78' : '#ffffff'}}>
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
  const [index, setIndex] = useState(0);
  const [status, setStatus] = useState({
    state: 'idle', position: 0, duration: 0, error: '', seekable: true, cover: '',
  });
  const [volume, setVolume] = useState(1);
  const trackRef = useRef(null);
  const indexRef = useRef(0);
  const skippingRef = useRef(false);
  trackRef.current = track;
  indexRef.current = index;

  const playIndex = useCallback(nextIndex => {
    const entry = TRACKS[nextIndex];
    try {
      const next = Music.play({path: entry.path, volume});
      setTrack(next);
      setIndex(nextIndex);
      setStatus({
        state: 'playing', position: 0, duration: 0, error: '', seekable: true, cover: '',
      });
    } catch (error) {
      setTrack(null);
      setStatus(previous => ({...previous, state: 'error', error: error.message}));
    }
  }, [volume]);

  useEffect(() => {
    if (!track) return undefined;
    const timer = setInterval(() => {
      try {
        const next = track.status();
        setStatus(next);
        const nearEnd = next.duration > 2 && next.position >= next.duration - 1.6;
        if (!skippingRef.current && (next.state === 'completed' || nearEnd)) {
          skippingRef.current = true;
          playIndex((indexRef.current + 1) % TRACKS.length);
          setTimeout(() => { skippingRef.current = false; }, 750);
        }
      } catch (error) {
        setStatus(previous => ({...previous, state: 'error', error: error.message}));
      }
    }, 250);
    return () => clearInterval(timer);
  }, [track, playIndex]);

  const play = () => playIndex(indexRef.current);
  const next = () => playIndex((indexRef.current + 1) % TRACKS.length);
  const previous = () => playIndex((indexRef.current - 1 + TRACKS.length) % TRACKS.length);
  const pause = () => { if (track) track.pause(); };
  const resume = () => { if (track) track.resume(); };
  const stop = () => { if (track) track.stop(); };
  const seekForward = () => {
    if (track && status.seekable) track.seek((status.position || 0) + 10);
  };

  const changeVolume = () => {
    const level = volume > 0.6 ? 0.4 : volume > 0.3 ? 1 : 0.2;
    setVolume(level);
    Music.setVolume(level);
  };

  const entry = TRACKS[index];
  const title = status.title || entry.label;
  const artist = status.artist || 'unknown artist';

  return (
    <FocusScope wrap>
      <View style={{
        width: 1920, height: 1080, backgroundColor: '#101820',
        alignItems: 'center', justifyContent: 'center', gap: 20,
      }}>
        <View style={{flexDirection: 'row', alignItems: 'center', gap: 34}}>
          {status.cover ? (
            <Image
              source={status.cover}
              resizeMode="cover"
              style={{width: 220, height: 220, borderRadius: 14}}
            />
          ) : (
            <View style={{
              width: 220, height: 220, borderRadius: 14,
              backgroundColor: '#182630', borderWidth: 2, borderColor: '#314451',
              alignItems: 'center', justifyContent: 'center',
            }}>
              <Text style={{fontFamily: font, fontSize: 22, color: '#5c6b78'}}>No cover</Text>
            </View>
          )}
          <View style={{gap: 10}}>
            <Text style={{fontFamily: font, fontSize: 62, color: '#ffffff'}}>
              PS4 React Music
            </Text>
            <Text style={{fontFamily: font, fontSize: 34, color: '#cfe0ff'}}>{title}</Text>
            <Text style={{fontFamily: font, fontSize: 26, color: '#8fa6b8'}}>
              {`${artist}  |  ${status.album || 'no album'}`}
            </Text>
          </View>
        </View>
        <Text style={{fontFamily: font, fontSize: 28, color: '#a0c4ff'}}>
          {`${status.state}  |  ${clock(status.position)} / ${clock(status.duration)}  |  ${entry.path}`}
        </Text>
        {status.error && (
          <Text style={{fontFamily: font, fontSize: 23, color: '#ff8a8a'}}>{status.error}</Text>
        )}
        <View style={{flexDirection: 'row', gap: 14}}>
          <Button label="Play" autoFocus onPress={play} />
          <Button label="Pause" disabled={!track} onPress={pause} />
          <Button label="Resume" disabled={!track} onPress={resume} />
          <Button label="Stop" disabled={!track} onPress={stop} />
        </View>
        <View style={{flexDirection: 'row', gap: 14}}>
          <Button label="Previous" onPress={previous} />
          <Button label="Next" onPress={next} />
          <Button label="Seek +10s" disabled={!track || !status.seekable} onPress={seekForward} />
          <Button label={`Volume: ${volume.toFixed(2)}`} onPress={changeVolume} />
        </View>
        <Text style={{fontFamily: font, fontSize: 21, color: '#5c6b78'}}>
          {`Serve the MP3/OGG at ${HTTP_SOURCE} and ${HTTP_OGG}, or edit the sources. Imported tick: ${tick}`}
        </Text>
      </View>
    </FocusScope>
  );
}

AppRegistry.registerComponent('sample-music', () => App);
