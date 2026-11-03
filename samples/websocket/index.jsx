import {useEffect, useState} from 'react';
import {AppRegistry, View, Text, FocusScope, WebSocket} from '@ps4-react/core';
import font from '../assets/Inter-Regular.ttf';

const URL = 'ws://192.168.0.8:8766/';

function Button({label, onPress, disabled, autoFocus}) {
  return (
    <View
      focusable
      autoFocus={autoFocus}
      onPress={disabled ? undefined : onPress}
      style={({focused}) => ({
        width: 360, height: 88, borderRadius: 12, borderWidth: 3,
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

function App() {
  const [socket, setSocket] = useState(null);
  const [state, setState] = useState('idle');
  const [error, setError] = useState('');
  const [messages, setMessages] = useState([]);

  useEffect(() => {
    if (!socket) return undefined;
    const timer = setInterval(() => {
      try {
        const result = socket.poll();
        setState(result.state);
        if (result.error) setError(result.error);
        if (result.events.length) {
          setMessages(previous => [...previous, ...result.events.map(event =>
            event.type === 'message' ? event.data : `[${event.type}]`)].slice(-10));
        }
      } catch (reason) {
        setError(reason.message || String(reason));
      }
    }, 250);
    return () => clearInterval(timer);
  }, [socket]);

  const connect = () => {
    setError('');
    setMessages([]);
    try {
      if (socket) socket.close();
      setSocket(WebSocket.connect(URL));
      setState('connecting');
    } catch (reason) {
      setError(reason.message || String(reason));
    }
  };

  const send = () => {
    if (!socket) return;
    const ok = socket.send(`hello ${Date.now()}`);
    setMessages(previous => [...previous, `-> hello (${ok ? 'sent' : 'failed'})`].slice(-10));
  };

  const close = () => {
    if (socket) socket.close();
    setState('closing');
  };

  return (
    <FocusScope wrap>
      <View style={{
        width: 1920, height: 1080, backgroundColor: '#101820',
        alignItems: 'center', justifyContent: 'center', gap: 24,
      }}>
        <Text style={{fontFamily: font, fontSize: 70, color: '#ffffff'}}>
          PS4 React WebSocket
        </Text>
        <Text style={{fontFamily: font, fontSize: 29, color: '#a0c4ff'}}>{`${state}  |  ${URL}`}</Text>
        {error && (
          <Text style={{fontFamily: font, fontSize: 24, color: '#ff8a8a'}}>{error}</Text>
        )}
        <View style={{width: 1400, minHeight: 300, gap: 6}}>
          {messages.length === 0 && (
            <Text style={{fontFamily: font, fontSize: 24, color: '#5c6b78'}}>
              Connect, then send a message. The local server echoes it.
            </Text>
          )}
          {messages.map((message, index) => (
            <Text key={index} style={{fontFamily: font, fontSize: 23, color: '#c8d6e5'}}>
              {message}
            </Text>
          ))}
        </View>
        <View style={{flexDirection: 'row', gap: 18}}>
          <Button label="Connect" autoFocus onPress={connect} />
          <Button label="Send hello" disabled={!socket} onPress={send} />
          <Button label="Close" disabled={!socket} onPress={close} />
        </View>
        <Text style={{fontFamily: font, fontSize: 22, color: '#5c6b78'}}>
          Local echo: ws://192.168.0.8:8766/
        </Text>
      </View>
    </FocusScope>
  );
}

AppRegistry.registerComponent('sample-websocket', () => App);
