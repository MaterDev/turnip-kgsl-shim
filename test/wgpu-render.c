#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "webgpu/webgpu.h"
#include "webgpu/wgpu.h"
static WGPUStringView S(const char*s){ WGPUStringView v={s,WGPU_STRLEN}; return v; }
static WGPUAdapter g_adapter; static WGPUDevice g_device; static int g_ad=0,g_dd=0,g_md=0; static WGPUMapAsyncStatus g_ms;
static void onAdapter(WGPURequestAdapterStatus st,WGPUAdapter a,WGPUStringView m,void*u1,void*u2){g_ad=1;if(st==WGPURequestAdapterStatus_Success)g_adapter=a;else printf("adapter fail %.*s\n",(int)m.length,m.data);}
static void onDevice(WGPURequestDeviceStatus st,WGPUDevice d,WGPUStringView m,void*u1,void*u2){g_dd=1;if(st==WGPURequestDeviceStatus_Success)g_device=d;else printf("device fail %.*s\n",(int)m.length,m.data);}
static void onMap(WGPUMapAsyncStatus st,WGPUStringView m,void*u1,void*u2){g_md=1;g_ms=st;if(st!=WGPUMapAsyncStatus_Success)printf("map fail %.*s\n",(int)m.length,m.data);}
static void onErr(WGPUDevice const*d,WGPUErrorType t,WGPUStringView m,void*u1,void*u2){printf("UNCAPTURED ERROR %d: %.*s\n",(int)t,(int)m.length,m.data);}
int main(){
  WGPUInstanceExtras ex; memset(&ex,0,sizeof ex); ex.chain.sType=(WGPUSType)WGPUSType_InstanceExtras; ex.backends=WGPUInstanceBackend_Vulkan;
  WGPUInstanceDescriptor id; memset(&id,0,sizeof id); id.nextInChain=(WGPUChainedStruct*)&ex;
  WGPUInstance inst=wgpuCreateInstance(&id); if(!inst){puts("no instance");return 1;}
  WGPURequestAdapterOptions o; memset(&o,0,sizeof o);
  WGPURequestAdapterCallbackInfo aci; memset(&aci,0,sizeof aci); aci.mode=WGPUCallbackMode_AllowProcessEvents; aci.callback=onAdapter;
  wgpuInstanceRequestAdapter(inst,&o,aci); for(int i=0;i<500&&!g_ad;i++) wgpuInstanceProcessEvents(inst);
  if(!g_adapter){puts("NO ADAPTER");return 2;}
  WGPUAdapterInfo info; memset(&info,0,sizeof info); wgpuAdapterGetInfo(g_adapter,&info); printf("adapter: %.*s (backend %d)\n",(int)info.device.length,info.device.data,(int)info.backendType);
  WGPUDeviceDescriptor dd; memset(&dd,0,sizeof dd); dd.label=S("thor"); dd.defaultQueue.label=S("q"); dd.uncapturedErrorCallbackInfo.callback=onErr;
  WGPURequestDeviceCallbackInfo dci; memset(&dci,0,sizeof dci); dci.mode=WGPUCallbackMode_AllowProcessEvents; dci.callback=onDevice;
  wgpuAdapterRequestDevice(g_adapter,&dd,dci); for(int i=0;i<500&&!g_dd;i++) wgpuInstanceProcessEvents(inst);
  if(!g_device){puts("NO DEVICE");return 3;}
  WGPUQueue q=wgpuDeviceGetQueue(g_device);
  const uint32_t W=64,H=64;
  WGPUTextureDescriptor td; memset(&td,0,sizeof td); td.usage=WGPUTextureUsage_RenderAttachment|WGPUTextureUsage_CopySrc; td.dimension=WGPUTextureDimension_2D; td.size.width=W; td.size.height=H; td.size.depthOrArrayLayers=1; td.format=WGPUTextureFormat_RGBA8Unorm; td.mipLevelCount=1; td.sampleCount=1;
  WGPUTexture tex=wgpuDeviceCreateTexture(g_device,&td); WGPUTextureView view=wgpuTextureCreateView(tex,NULL);
  const char* wgsl="@vertex fn vs(@builtin(vertex_index) i:u32)->@builtin(position) vec4f{var p=array<vec2f,3>(vec2f(-1,-3),vec2f(-1,1),vec2f(3,1));return vec4f(p[i],0,1);}\n@fragment fn fs(@builtin(position) p:vec4f)->@location(0) vec4f{return vec4f(p.x/64.0,p.y/64.0,0.5,1.0);}";
  WGPUShaderSourceWGSL src; memset(&src,0,sizeof src); src.chain.sType=WGPUSType_ShaderSourceWGSL; src.code=S(wgsl);
  WGPUShaderModuleDescriptor sd; memset(&sd,0,sizeof sd); sd.nextInChain=&src.chain; WGPUShaderModule sm=wgpuDeviceCreateShaderModule(g_device,&sd);
  WGPUColorTargetState ct; memset(&ct,0,sizeof ct); ct.format=WGPUTextureFormat_RGBA8Unorm; ct.writeMask=WGPUColorWriteMask_All;
  WGPUFragmentState fs; memset(&fs,0,sizeof fs); fs.module=sm; fs.entryPoint=S("fs"); fs.targetCount=1; fs.targets=&ct;
  WGPURenderPipelineDescriptor pd; memset(&pd,0,sizeof pd); pd.vertex.module=sm; pd.vertex.entryPoint=S("vs"); pd.primitive.topology=WGPUPrimitiveTopology_TriangleList; pd.multisample.count=1; pd.multisample.mask=0xFFFFFFFF; pd.fragment=&fs;
  WGPURenderPipeline pipe=wgpuDeviceCreateRenderPipeline(g_device,&pd); if(!pipe){puts("NO PIPELINE");return 4;}
  const uint32_t bpr=256; /* 64*4=256, already aligned */
  WGPUBufferDescriptor bd; memset(&bd,0,sizeof bd); bd.usage=WGPUBufferUsage_MapRead|WGPUBufferUsage_CopyDst; bd.size=bpr*H; WGPUBuffer buf=wgpuDeviceCreateBuffer(g_device,&bd);
  WGPUCommandEncoder enc=wgpuDeviceCreateCommandEncoder(g_device,NULL);
  WGPURenderPassColorAttachment ca; memset(&ca,0,sizeof ca); ca.view=view; ca.depthSlice=WGPU_DEPTH_SLICE_UNDEFINED; ca.loadOp=WGPULoadOp_Clear; ca.storeOp=WGPUStoreOp_Store; ca.clearValue.a=1;
  WGPURenderPassDescriptor rp; memset(&rp,0,sizeof rp); rp.colorAttachmentCount=1; rp.colorAttachments=&ca;
  WGPURenderPassEncoder pass=wgpuCommandEncoderBeginRenderPass(enc,&rp); wgpuRenderPassEncoderSetPipeline(pass,pipe); wgpuRenderPassEncoderDraw(pass,3,1,0,0); wgpuRenderPassEncoderEnd(pass);
  WGPUTexelCopyTextureInfo srcT; memset(&srcT,0,sizeof srcT); srcT.texture=tex; WGPUTexelCopyBufferInfo dstB; memset(&dstB,0,sizeof dstB); dstB.buffer=buf; dstB.layout.bytesPerRow=bpr; dstB.layout.rowsPerImage=H; WGPUExtent3D ext={W,H,1};
  wgpuCommandEncoderCopyTextureToBuffer(enc,&srcT,&dstB,&ext);
  WGPUCommandBuffer cb=wgpuCommandEncoderFinish(enc,NULL); wgpuQueueSubmit(q,1,&cb);
  WGPUBufferMapCallbackInfo mci; memset(&mci,0,sizeof mci); mci.mode=WGPUCallbackMode_AllowProcessEvents; mci.callback=onMap;
  wgpuBufferMapAsync(buf,WGPUMapMode_Read,0,bpr*H,mci);
  for(int i=0;i<2000&&!g_md;i++){ wgpuDevicePoll(g_device,1,NULL); wgpuInstanceProcessEvents(inst);} if(!g_md){puts("MAP TIMEOUT");return 5;}
  const unsigned char* px=(const unsigned char*)wgpuBufferGetConstMappedRange(buf,0,bpr*H);
  printf("pixel(0,0)=%u,%u,%u,%u  pixel(63,63)=%u,%u,%u,%u  (expect ~0,0,128,255 and ~255,255,128,255)\n",px[0],px[1],px[2],px[3],px[63*bpr+63*4],px[63*bpr+63*4+1],px[63*bpr+63*4+2],px[63*bpr+63*4+3]);
  int ok = px[2]>120&&px[2]<136 && px[63*bpr+63*4]>240 && px[63*bpr+63*4+1]>240;
  printf("RESULT: %s\n", ok?"GPU-RENDER-OK":"WRONG-PIXELS");
  return ok?0:6; }
