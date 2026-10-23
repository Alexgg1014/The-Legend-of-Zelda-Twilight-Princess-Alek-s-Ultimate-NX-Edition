// Prints the byte layout of the pipeline recipes (what pipeline_cache.db stores with memcpy),
// to compare across ABIs: if x86_64 and aarch64 agree byte for byte, a PC harvest is valid
// on the console.
#include "gx/pipeline.hpp"
#include "gfx/clear.hpp"
#include "rmlui/pipeline.hpp"
#include <cstddef>
#include <cstdio>
using namespace aurora;
#define F(T, m) std::printf("%s.%s %zu %zu\n", #T, #m, offsetof(T, m), sizeof(T::m))
int main() {
  std::printf("sizeof gx::PipelineConfig %zu\n", sizeof(gx::PipelineConfig));
  std::printf("sizeof gx::ShaderConfig %zu\n", sizeof(gx::ShaderConfig));
  std::printf("sizeof clear::PipelineConfig %zu\n", sizeof(gfx::clear::PipelineConfig));
  std::printf("sizeof rmlui::PipelineConfig %zu\n", sizeof(rmlui::PipelineConfig));
  F(gx::PipelineConfig, version); F(gx::PipelineConfig, shaderConfig); F(gx::PipelineConfig, depthFunc);
  F(gx::PipelineConfig, cullMode); F(gx::PipelineConfig, blendMode); F(gx::PipelineConfig, blendFacSrc);
  F(gx::PipelineConfig, blendFacDst); F(gx::PipelineConfig, blendOp); F(gx::PipelineConfig, dstAlpha);
  F(gx::PipelineConfig, polygonOffsetBits); F(gx::PipelineConfig, polygonOffsetClampBits);
  F(gx::PipelineConfig, depthCompare); F(gx::PipelineConfig, colorUpdate);
  F(gx::ShaderConfig, attrs); F(gx::ShaderConfig, tevSwapTable); F(gx::ShaderConfig, tevStages);
  F(gx::ShaderConfig, tevStageCount); F(gx::ShaderConfig, colorChannels); F(gx::ShaderConfig, tcgs);
  F(gx::ShaderConfig, alphaCompare); F(gx::ShaderConfig, indStages); F(gx::ShaderConfig, numIndStages);
  // Bit-field placement: set each bit-field alone and print the byte it lands in.
  gx::ShaderConfig s{};
  s.lineMode = 3;
  std::printf("bitfield lineMode=3 -> byte2 0x%02x\n", reinterpret_cast<unsigned char*>(&s)[2]);
  s = {};
  s.fogRangeEnabled = 1;
  std::printf("bitfield fogRangeEnabled=1 -> byte2 0x%02x\n", reinterpret_cast<unsigned char*>(&s)[2]);
}
