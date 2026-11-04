import {useState} from 'react';
import {AppRegistry, View, Text, Image, ScrollView, FocusScope} from '@ps4-react/core';
import font from '../assets/Inter-Regular.ttf';
import image01 from './assets/gallery-01.png';
import image02 from './assets/gallery-02.png';
import image03 from './assets/gallery-03.png';
import image04 from './assets/gallery-04.png';
import image05 from './assets/gallery-05.png';
import image06 from './assets/gallery-06.png';
import image07 from './assets/gallery-07.png';
import image08 from './assets/gallery-08.png';
import image09 from './assets/gallery-09.png';

const IMAGES = [image01, image02, image03, image04, image05, image06, image07, image08, image09];
const ITEMS = IMAGES.map((image, index) => ({
  image,
  title: `Gallery ${String(index + 1).padStart(2, '0')}`,
  subtitle: index % 2 ? 'gradient + rings' : 'gradient + horizon',
}));

function Card({item, selected, onPress, autoFocus}) {
  return (
    <View
      focusable
      autoFocus={autoFocus}
      onPress={onPress}
      style={({focused}) => ({
        width: 500, height: 340, padding: 16, gap: 12, borderRadius: 14,
        borderWidth: 3,
        borderColor: selected ? '#7fd1c9' : focused ? '#a0c4ff' : '#263947',
        backgroundColor: focused ? '#1f3140' : selected ? '#172d2c' : '#141f27',
      })}>
      <Image
        source={item.image}
        resizeMode="cover"
        style={{width: 468, height: 263, borderRadius: 8, backgroundColor: '#0b1116'}}
      />
      <View style={{flexDirection: 'row', alignItems: 'baseline', gap: 12}}>
        <Text style={{fontFamily: font, fontSize: 26, color: '#ffffff'}}>{item.title}</Text>
        <Text style={{fontFamily: font, fontSize: 20, color: '#7f9db8'}}>{item.subtitle}</Text>
      </View>
    </View>
  );
}

function App() {
  const [selected, setSelected] = useState(0);

  return (
    <FocusScope wrap>
      <ScrollView style={{width: 1920, height: 1080, backgroundColor: '#0b1116'}}>
        <View style={{width: 1920, paddingHorizontal: 56, paddingVertical: 42, gap: 24}}>
          <View style={{gap: 6}}>
            <Text style={{fontFamily: font, fontSize: 58, color: '#ffffff'}}>
              PS4 React Gallery
            </Text>
            <Text style={{fontFamily: font, fontSize: 24, color: '#7f9db8'}}>
              {`ScrollView + baked PNGs · ${ITEMS[selected].title} selected · D-pad scrolls the focused card into view`}
            </Text>
          </View>
          <View style={{flexDirection: 'row', flexWrap: 'wrap', gap: 24}}>
            {ITEMS.map((item, index) => (
              <Card
                key={item.title}
                item={item}
                selected={selected === index}
                autoFocus={index === 0}
                onPress={() => setSelected(index)}
              />
            ))}
          </View>
          <Text style={{fontFamily: font, fontSize: 21, color: '#5c6b78'}}>
            Nine bundled images, three columns, focus-driven scrolling.
          </Text>
        </View>
      </ScrollView>
    </FocusScope>
  );
}

AppRegistry.registerComponent('sample-gallery', () => App);
