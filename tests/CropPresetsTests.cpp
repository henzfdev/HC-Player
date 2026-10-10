#include "../HC Player/CropPresets.h"
#include <cassert>
#include <limits>
#include <iterator>
int main() {
 using namespace hc::crop;
 static_assert(PresetCount == 18);
 assert(NextPreset(0, false) == 1);
 assert(NextPreset(PresetCount - 1, false) == 0);
 assert(NextPreset(0, true) == PresetCount - 1);
 for(int i=0;i<PresetCount;i++)assert(NextPreset(NextPreset(i,false),true)==i);
 for(int i=1;i<PresetCount;i++)assert(Presets[i].aspect>Presets[i-1].aspect);
 assert((Dimensions(1920,1080,1,0,2.39)==std::pair(1920,803)));
 assert((Dimensions(1920,1080,1,0,4.0/3.0)==std::pair(1440,1080)));
 assert((Dimensions(720,576,64.0/45.0,0,4.0/3.0)==std::pair(540,576)));
 assert((Dimensions(1080,1920,1,90,2.39)==std::pair(803,1920)));
 assert((Dimensions(0,1080,1,0,2.39)==std::pair(0,0)));
 assert((Dimensions(1920,1080,std::numeric_limits<double>::quiet_NaN(),0,2.39)==std::pair(0,0)));
 for(auto [w,h]: {std::pair(3840,2160),std::pair(640,480),std::pair(1080,1920)})for(int i=1;i<PresetCount;i++){auto [cw,ch]=Dimensions(w,h,1,0,Presets[i].aspect);assert(cw>0&&cw<=w&&ch>0&&ch<=h);}
}
