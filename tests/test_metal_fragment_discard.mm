// Native Metal diagnostic: discard must suppress every MRT, including outputs
// assigned before the discard. Run with MTL_SHADER_VALIDATION=0 and =1.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <cstdio>

static const char* source = R"MSL(
#include <metal_stdlib>
using namespace metal;
struct V { float4 position [[position]]; };
vertex V vs(uint id [[vertex_id]]) {
    float2 p[] = {float2(-1,-1), float2(3,-1), float2(-1,3)};
    return {float4(p[id], 0.5, 1)};
}
struct F {
    float4 a [[color(0)]]; float4 b [[color(1)]];
    float4 c [[color(2)]]; float4 d [[color(3)]];
    float4 e [[color(4)]]; float4 f [[color(5)]];
};
fragment F fs(V in [[stage_in]]) {
    bool killed = (uint(in.position.x) & 1) == 0;
    F out = {};
    float4 value;
    if (!killed) value = float4(0,1,0,1);
    else value = float4(0,0,in.position.y/32,0);
    out.b = out.c = out.d = out.e = out.f = value;
    if (killed) { discard_fragment(); }
    out.a = value;
    return out;
}
)MSL";

int main() {
    @autoreleasepool {
        id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        if (!device) return 2;
        NSError* error = nil;
        MTLCompileOptions* options = [MTLCompileOptions new];
        options.fastMathEnabled = NO;
        id<MTLLibrary> library = [device newLibraryWithSource:@(source) options:options error:&error];
        if (!library) { NSLog(@"%@", error); return 2; }
        MTLRenderPipelineDescriptor* pd = [MTLRenderPipelineDescriptor new];
        pd.vertexFunction = [library newFunctionWithName:@"vs"];
        pd.fragmentFunction = [library newFunctionWithName:@"fs"];
        pd.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float_Stencil8;
        pd.stencilAttachmentPixelFormat = MTLPixelFormatDepth32Float_Stencil8;
        for (int i = 0; i < 6; ++i)
            pd.colorAttachments[i].pixelFormat = MTLPixelFormatRGBA8Unorm;
        id<MTLRenderPipelineState> pipeline = [device newRenderPipelineStateWithDescriptor:pd error:&error];
        if (!pipeline) { NSLog(@"%@", error); return 2; }
        MTLTextureDescriptor* td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
            width:32 height:32 mipmapped:NO];
        td.usage = MTLTextureUsageRenderTarget;
        td.storageMode = MTLStorageModeShared;
        id<MTLTexture> targets[6];
        MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
        for (int i = 0; i < 6; ++i) {
            targets[i] = [device newTextureWithDescriptor:td];
            pass.colorAttachments[i].texture = targets[i];
            pass.colorAttachments[i].loadAction = MTLLoadActionClear;
            pass.colorAttachments[i].storeAction = MTLStoreActionStore;
            pass.colorAttachments[i].clearColor = MTLClearColorMake(1,0,0,1);
        }
        td.pixelFormat = MTLPixelFormatDepth32Float_Stencil8;
        td.storageMode = MTLStorageModePrivate;
        id<MTLTexture> depth = [device newTextureWithDescriptor:td];
        pass.depthAttachment.texture = depth;
        pass.depthAttachment.loadAction = MTLLoadActionClear;
        pass.depthAttachment.clearDepth = 1;
        pass.stencilAttachment.texture = depth;
        pass.stencilAttachment.loadAction = MTLLoadActionClear;
        MTLDepthStencilDescriptor* ds = [MTLDepthStencilDescriptor new];
        ds.depthCompareFunction = MTLCompareFunctionLessEqual;
        ds.depthWriteEnabled = YES;
        id<MTLCommandQueue> queue = [device newCommandQueue];
        id<MTLCommandBuffer> command = [queue commandBuffer];
        id<MTLRenderCommandEncoder> encoder = [command renderCommandEncoderWithDescriptor:pass];
        [encoder setRenderPipelineState:pipeline];
        [encoder setDepthStencilState:[device newDepthStencilStateWithDescriptor:ds]];
        [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
        [encoder endEncoding];
        [command commit];
        [command waitUntilCompleted];
        if (command.error) { NSLog(@"%@", command.error); return 2; }
        int failures = 0;
        for (int i = 0; i < 6; ++i) {
            unsigned char bytes[32*32*4];
            [targets[i] getBytes:bytes bytesPerRow:32*4 fromRegion:MTLRegionMake2D(0,0,32,32) mipmapLevel:0];
            int bad = 0;
            for (int y = 0; y < 32; ++y) for (int x = 0; x < 32; ++x) {
                const auto* pixel = &bytes[(y*32+x)*4];
                const bool killed = (x & 1) == 0;
                bad += pixel[0] != (killed ? 255 : 0) || pixel[1] != (killed ? 0 : 255)
                    || pixel[2] != 0 || pixel[3] != 255;
            }
            std::printf("MRT %d: %d incorrect pixels\n", i, bad);
            failures += bad;
        }
        return failures ? 1 : 0;
    }
}
