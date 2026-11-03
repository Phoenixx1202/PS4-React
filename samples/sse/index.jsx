import {useEffect, useState} from 'react';
import {AppRegistry, View, Text, FocusScope, Keyboard, Sse} from '@ps4-react/core';
import font from '../assets/Inter-Regular.ttf';

const DEFAULT_URL = 'https://postman-echo.com/server-events/10';

function Button({label, onPress, autoFocus}) {
  return (
    <View
      focusable
      autoFocus={autoFocus}
      onPress={onPress}
      style={({focused}) => ({
        width: 360, height: 88, borderRadius: 12, borderWidth: 3,
        borderColor: focused ? '#a0c4ff' : '#314451',
        backgroundColor: focused ? '#294559' : '#182630',
        alignItems: 'center', justifyContent: 'center',
      })}>
      <Text style={{fontFamily: font, fontSize: 32, color: '#ffffff'}}>{label}</Text>
    </View>
  );
}

function App() {
  const [url, setUrl] = useState(DEFAULT_URL);
  const [stream, setStream] = useState(null);
  const [state, setState] = useState('idle');
  const [error, setError] = useState('');
  const [events, setEvents] = useState([]);

  useEffect(() => {
    if (!stream) return undefined;
    const timer = setInterval(() => {
      try {
        const result = stream.poll();
        setState(result.state);
        if (result.error) setError(result.error);
        if (result.events.length) {
          setEvents(previous => [...previous, ...result.events].slice(-8));
        }
      } catch (reason) {
        setError(reason.message || String(reason));
      }
    }, 250);
    return () => clearInterval(timer);
  }, [stream]);

  const open = target => {
    setError('');
    setEvents([]);
    try {
      if (stream) stream.close();
      const next = Sse.connect({url: target, headers: ['Accept: text/event-stream']});
      setUrl(target);
      setStream(next);
      setState('connecting');
    } catch (reason) {
      setError(reason.message || String(reason));
    }
  };

  const connect = () => open(url);
  const local = () => open('http://192.168.0.8:8765/events');

  const close = () => {
    if (stream) stream.close();
    setState('closing');
  };

  const edit = async () => {
    try {
      const result = await Keyboard.prompt({
        title: 'SSE URL', placeholder: 'https://host/events', value: url, maxLength: 256,
      });
      if (result.text) setUrl(result.text);
    } catch {
      // Keyboard cancelled.
    }
  };

  return (
    <FocusScope wrap>
      <View style={{
        width: 1920, height: 1080, backgroundColor: '#101820',
        alignItems: 'center', justifyContent: 'center', gap: 22,
      }}>
        <Text style={{fontFamily: font, fontSize: 70, color: '#ffffff'}}>
          PS4 React SSE
        </Text>
        <Text style={{fontFamily: font, fontSize: 29, color: '#a0c4ff'}}>
          {`${state}  |  ${url}`}
        </Text>
        {error && (
          <Text style={{fontFamily: font, fontSize: 24, color: '#ff8a8a'}}>{error}</Text>
        )}
        <View style={{width: 1400, minHeight: 320, gap: 6}}>
          {events.length === 0 && (
            <Text style={{fontFamily: font, fontSize: 24, color: '#5c6b78'}}>
              Waiting for events...
            </Text>
          )}
          {events.map((event, index) => (
            <Text key={`${event.id}-${index}`} style={{fontFamily: font, fontSize: 22, color: '#c8d6e5'}}>
              {`[${event.type}] ${event.data}`}
            </Text>
          ))}
        </View>
        <View style={{flexDirection: 'row', gap: 18}}>
          <Button label="Connect" autoFocus onPress={connect} />
          <Button label="Local test" onPress={local} />
          <Button label="Close" onPress={close} />
          <Button label="Change URL" onPress={edit} />
        </View>
        <Text style={{fontFamily: font, fontSize: 22, color: '#5c6b78'}}>
          Default: postman-echo.com/server-events/10
        </Text>
      </View>
    </FocusScope>
  );
}

AppRegistry.registerComponent('sample-sse', () => App);
