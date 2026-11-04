import {useCallback, useState} from 'react';
import {AppRegistry, View, Text, ScrollView, VirtualList, FocusScope} from '@ps4-react/core';
import font from '../assets/Inter-Regular.ttf';

const DATA = Array.from({length: 1000}, (_, index) => ({
  id: index + 1,
  title: `Item ${String(index + 1).padStart(4, '0')}`,
  detail: index % 3 === 0 ? 'windowed row' : index % 3 === 1 ? 'focus scroll' : 'baked font',
}));

function App() {
  const [selected, setSelected] = useState(1);

  const renderItem = useCallback(({item, index}) => (
    <View
      focusable
      onPress={() => setSelected(item.id)}
      style={({focused}) => ({
        flex: 1, height: 86, flexDirection: 'row', alignItems: 'center',
        justifyContent: 'space-between', paddingHorizontal: 26, borderRadius: 12,
        borderWidth: 2,
        borderColor: selected === item.id ? '#7fd1c9' : focused ? '#a0c4ff' : '#263947',
        backgroundColor: focused ? '#1f3140' : selected === item.id ? '#172d2c' : '#141f27',
      })}>
      <View style={{gap: 2}}>
        <Text style={{fontFamily: font, fontSize: 28, color: '#ffffff'}}>{item.title}</Text>
        <Text style={{fontFamily: font, fontSize: 19, color: '#7f9db8'}}>{item.detail}</Text>
      </View>
      <Text style={{fontFamily: font, fontSize: 22, color: '#5c8fb8'}}>
        {String(index + 1).padStart(4, '0')}
      </Text>
    </View>
  ), [selected]);

  const keyExtractor = useCallback(item => String(item.id), []);

  return (
    <FocusScope wrap>
      <ScrollView style={{width: 1920, height: 1080, backgroundColor: '#0b1116'}}>
        <View style={{width: 1920, paddingHorizontal: 56, paddingVertical: 42, gap: 18}}>
          <Text style={{fontFamily: font, fontSize: 58, color: '#ffffff'}}>
            PS4 React List
          </Text>
          <Text style={{fontFamily: font, fontSize: 24, color: '#7f9db8'}}>
            {`VirtualList · 1000 rows · selected ${String(selected).padStart(4, '0')} · only the rows around the viewport are mounted`}
          </Text>
          <VirtualList
            data={DATA}
            renderItem={renderItem}
            keyExtractor={keyExtractor}
            itemHeight={86}
            rowGap={10}
            numColumns={1}
            initialNumRows={14}
            overscan={4}
            style={{width: 1808}}
          />
          <Text style={{fontFamily: font, fontSize: 21, color: '#5c6b78'}}>
            D-pad scrolls the focused row into view; the list re-centres its window near the ends.
          </Text>
        </View>
      </ScrollView>
    </FocusScope>
  );
}

AppRegistry.registerComponent('sample-list', () => App);
