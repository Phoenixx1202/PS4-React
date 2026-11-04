import {AppRegistry, View, Text} from '@ps4-react/core';
import font from '../assets/Inter-Regular.ttf';

function Row({label, children, size = 36}) {
  return (
    <View style={{flexDirection: 'row', alignItems: 'baseline', gap: 18}}>
      <Text style={{fontFamily: font, fontSize: 22, color: '#5c8fb8', width: 170}}>
        {label}
      </Text>
      <Text style={{fontFamily: font, fontSize: size, color: '#ffffff'}}>{children}</Text>
    </View>
  );
}

function App() {
  return (
    <View style={{
      width: 1920, height: 1080, backgroundColor: '#0b1116',
      padding: 64, gap: 14,
    }}>
      <Text style={{fontFamily: font, fontSize: 58, color: '#ffffff'}}>
        PS4 React Text
      </Text>
      <Text style={{fontFamily: font, fontSize: 24, color: '#7f9db8'}}>
        HarfBuzz + SheenBidi runtime shaping | fonts in /app0/fonts
      </Text>
      <View style={{height: 2, backgroundColor: '#1f2d38', marginVertical: 12}} />
      <Row label="Japanese">日本語 こんにちは世界</Row>
      <Row label="Chinese">中文 你好，世界</Row>
      <Row label="Arabic">العربية: مرحبا بالعالم</Row>
      <Row label="Hindi">हिन्दी: नमस्ते दुनिया</Row>
      <Row label="Bengali">বাংলা: হ্যালো বিশ্ব</Row>
      <View style={{height: 2, backgroundColor: '#1f2d38', marginVertical: 12}} />
      <Text style={{
        fontFamily: font, fontSize: 26, color: '#c9d6e2',
        width: 1700, lineHeight: 40,
      }}>
        这是一个很长的段落，用来测试中文的自动换行和标点规则。日本語の文章もここで折り返されます。العربية تكتب من اليمين إلى اليسار وتختبر الترتيب ثنائي الاتجاه داخل سطر طويل.
      </Text>
      <Text style={{fontFamily: font, fontSize: 22, color: '#5c6b78'}}>
        Latin text stays on the baked Inter font; the shaper only handles missing glyphs and complex scripts.
      </Text>
    </View>
  );
}

AppRegistry.registerComponent('sample-text', () => App);
