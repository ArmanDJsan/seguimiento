/**
 * VertexShader.hlsl
 * 
 * Simple vertex shader for full-screen quad rendering
 * Used to apply the pixel shader across the entire texture
 */

struct VSInput {
    float4 position : POSITION;
    float2 texCoord : TEXCOORD0;
};

struct VSOutput {
    float4 position : SV_POSITION;
    float2 texCoord : TEXCOORD0;
};

VSOutput main(VSInput input) {
    VSOutput output;
    output.position = input.position;
    output.texCoord = input.texCoord;
    return output;
}
