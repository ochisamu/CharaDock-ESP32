// Host-only monochrome framebuffer for renderer regression tests. No hardware.
#pragma once
#include <array>
#include <cmath>
#include <cstring>
inline const unsigned char u8g2_font_logisoso32_tn[] = {0};
class U8G2 {
public:
  std::array<unsigned char, 400 * 300> pixels{};
  int color = 1;
  void clearBuffer() { pixels.fill(0); }
  void setDrawColor(int c) { color = c; }
  void drawPixel(int x, int y) { if (x >= 0 && x < 400 && y >= 0 && y < 300) pixels[y*400+x] = color; }
  void drawBox(int x,int y,int w,int h) { for(int j=0;j<h;j++)for(int i=0;i<w;i++)drawPixel(x+i,y+j); }
  void drawHLine(int x,int y,int w) { drawBox(x,y,w,1); }
  void drawVLine(int x,int y,int h) { drawBox(x,y,1,h); }
  void drawFrame(int x,int y,int w,int h) { drawHLine(x,y,w);drawHLine(x,y+h-1,w);drawVLine(x,y,h);drawVLine(x+w-1,y,h); }
  void drawDisc(int x,int y,int r) { for(int j=-r;j<=r;j++)for(int i=-r;i<=r;i++)if(i*i+j*j<=r*r)drawPixel(x+i,y+j); }
  void drawCircle(int x,int y,int r) { for(int d=0;d<360;d++)drawPixel(x+std::lround(r*std::cos(d*0.0174533)),y+std::lround(r*std::sin(d*0.0174533))); }
  void drawLine(int x,int y,int a,int b) { int n=std::max(std::abs(a-x),std::abs(b-y));for(int i=0;i<=n;i++)drawPixel(x+(a-x)*i/std::max(1,n),y+(b-y)*i/std::max(1,n)); }
  void setFont(const unsigned char*) {}
  int getStrWidth(const char* s) { return std::strlen(s)*16; }
  void drawStr(int,int,const char*) {} // Ambient clock is not exercised here.
};
