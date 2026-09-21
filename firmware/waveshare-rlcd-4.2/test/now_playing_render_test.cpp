// Run with the real renderer/fonts and a host-only framebuffer; no serial I/O.
#include <cassert>
#include <fstream>
#include <iterator>
#include <vector>
#include <string>
#include "charadock/scene_renderer.hpp"
#include "charadock/protocol_v2.hpp"
using namespace charadock::rlcd;
std::vector<uint8_t> read(const std::string& path) {
  std::ifstream f(path, std::ios::binary); assert(f.good());
  return {std::istreambuf_iterator<char>(f), {}};
}
void save(const U8G2& c, const std::string& path) {
  std::ofstream f(path,std::ios::binary); f << "P5\n400 300\n255\n";
  for(auto p:c.pixels)f.put(p?0:255);
}
int ink(const U8G2& c,int y,int h){int n=0;for(int j=y;j<y+h;j++)for(int x=12;x<388;x++)n+=c.pixels[j*400+x];return n;}
int main(int argc,char**argv) {
  assert(argc==4); // firmware directory, raw 400x300 MSB portrait, output prefix
  const std::string root=argv[1], output=argv[3];
  auto small=read(root+"/src/generated/shinonome12.bin"), large=read(root+"/src/generated/shinonome16.bin");
  SceneRenderer renderer;assert(renderer.attachFonts({small.data(),small.size(),large.data(),large.size()}));
  auto image=read(argv[2]); assert(image.size()==15000);
  uint8_t first[15000],second[15000]; MonochromeAssetStore portrait;
  assert(portrait.attach(first,second,15000));
  MonochromeAssetMetadata m;m.width=400;m.height=300;m.byteCount=15000;m.revision[0]='1';m.frameName[0]='p';
  m.checksum=charadock::protocol::crc32(image.data(),image.size()) ^ 0xffffffffu;
  assert(portrait.beginTransfer(m)==MonochromeAssetResult::Ok);
  assert(portrait.writeChunk(0,image.data(),image.size())==MonochromeAssetResult::Ok);
  assert(portrait.finishTransfer()==MonochromeAssetResult::Ok);
  SceneSnapshot scene;scene.scene=SceneId::Home;scene.state=DeviceState::Idle;scene.modeLabel="CHAT";scene.flags=SceneConnected;scene.characterName="コハク";
  scene.footer="再生中\n夕暮れの街を歩きながら聴きたい、あなたと私の小さな物語\nサンプルアーティスト";
  SensorSnapshot sensors;U8G2 c;
  renderer.compose(c,scene,sensors,portrait);save(c,output+"-playing.pgm");
  assert(ink(c,246,16)>0);assert(ink(c,266,16)>0);assert(ink(c,283,12)>0);
  scene.footer="一時停止\n夕暮れの街を歩きながら聴きたい、あなたと私の小さな物語\nサンプルアーティスト";
  renderer.compose(c,scene,sensors,portrait);save(c,output+"-paused.pgm");
  scene.footer="USB接続  CharaDock";
  renderer.compose(c,scene,sensors,portrait);save(c,output+"-cleared.pgm");
  const auto cleared=c.pixels;
  SceneRenderer fresh;assert(fresh.attachFonts({small.data(),small.size(),large.data(),large.size()}));
  fresh.compose(c,scene,sensors,portrait);assert(c.pixels==cleared);
  scene.scene=SceneId::Conversation;scene.caption="会話中は、曲の表示よりも字幕を優先します。";
  scene.footer="再生中\n秘密のタイトル\nアーティスト";
  renderer.compose(c,scene,sensors,portrait);const auto talking=c.pixels;
  scene.footer.clear();renderer.compose(c,scene,sensors,portrait);assert(c.pixels==talking);
  save(c,output+"-conversation.pgm");
}
