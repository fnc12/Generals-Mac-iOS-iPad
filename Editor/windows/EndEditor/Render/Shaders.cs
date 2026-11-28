namespace EndEditor.Render;

/// HLSL of the map view and the model preview: the Metal shaders of the macOS client
/// (MapView.swift, ModelPreview.swift) ported. Matrices from C# (System.Numerics) are row-major
/// and multiply row vectors; the preview's bone matrices from the core are column-major.
internal static class Shaders
{
    public const string Map = """
        cbuffer Frame : register(b0)
        {
            row_major float4x4 viewProjection;
            float4 terrainToLight;
            float4 terrainAmbient;
            float4 terrainDiffuse;
            float4 objectToLight;
            float4 objectAmbient;
            float4 objectDiffuse;
            float4 playable;  // x0 y0 x1 y1
            float4 water;     // rgb tint, a opacity
            float4 params;    // x: world units per water texture repeat
        };

        cbuffer Part : register(b1)
        {
            uint4 flags;      // x: textured, y: alpha test, z: lit, w: player color (1 texture alpha, 2 tint)
            float4 baseColor;
            float4 rect;      // labels and the rubber band: x0 y0 x1 y1 in clip space
        };

        Texture2D colorTexture : register(t0);
        SamplerState colorSampler : register(s0);

        struct TerrainIn
        {
            float3 position : POSITION;
            float3 normal : NORMAL;
            float2 uv : TEXCOORD0;
            float2 blendUv : TEXCOORD1;
            float2 extraUv : TEXCOORD2;
            float2 alpha : TEXCOORD3;
        };

        struct TerrainOut
        {
            float4 position : SV_Position;
            float3 world : TEXCOORD4;
            float3 normal : NORMAL;
            float2 uv : TEXCOORD0;
            float2 blendUv : TEXCOORD1;
            float2 extraUv : TEXCOORD2;
            float2 alpha : TEXCOORD3;
        };

        TerrainOut terrainVertex(TerrainIn i)
        {
            TerrainOut o;
            o.position = mul(float4(i.position, 1.0), viewProjection);
            o.world = i.position;
            o.normal = i.normal;
            o.uv = i.uv;
            o.blendUv = i.blendUv;
            o.extraUv = i.extraUv;
            o.alpha = i.alpha;
            return o;
        }

        float4 terrainPixel(TerrainOut i) : SV_Target
        {
            float3 color = colorTexture.Sample(colorSampler, i.uv).rgb;
            color = lerp(color, colorTexture.Sample(colorSampler, i.blendUv).rgb, i.alpha.x);
            color = lerp(color, colorTexture.Sample(colorSampler, i.extraUv).rgb, i.alpha.y);
            float light = max(dot(normalize(i.normal), terrainToLight.xyz), 0.0);
            color *= terrainAmbient.rgb + terrainDiffuse.rgb * light;
            // Outside the playable area, as WorldBuilder shades the border.
            if (i.world.x < playable.x || i.world.y < playable.y || i.world.x > playable.z || i.world.y > playable.w)
                color *= 0.55;
            return float4(color, 1.0);
        }

        struct ModelIn
        {
            float3 position : POSITION;
            float3 normal : NORMAL;
            float2 uv : TEXCOORD0;
            float4 row0 : INSTANCE0;
            float4 row1 : INSTANCE1;
            float4 row2 : INSTANCE2;
            float4 row3 : INSTANCE3;
            float4 color : COLOR;
        };

        struct ModelOut
        {
            float4 position : SV_Position;
            float3 normal : NORMAL;
            float2 uv : TEXCOORD0;
            float4 color : COLOR;
        };

        ModelOut modelVertex(ModelIn i)
        {
            float4x4 transform = float4x4(i.row0, i.row1, i.row2, i.row3);
            ModelOut o;
            o.position = mul(mul(float4(i.position, 1.0), transform), viewProjection);
            o.normal = mul(float4(i.normal, 0.0), transform).xyz;
            o.uv = i.uv;
            o.color = i.color;
            return o;
        }

        float4 modelPixel(ModelOut i) : SV_Target
        {
            float4 color = baseColor;
            if (flags.x != 0)
                color = colorTexture.Sample(colorSampler, i.uv);
            if (flags.w == 1)
                color = float4(lerp(i.color.rgb, color.rgb, color.a), 1.0);
            else if (flags.w == 2)
                color.rgb *= i.color.rgb;
            if (flags.y != 0 && color.a < 0.375)
                discard;
            if (flags.z != 0)
            {
                float light = abs(dot(normalize(i.normal), objectToLight.xyz));
                color.rgb *= objectAmbient.rgb + objectDiffuse.rgb * light;
            }
            return color;
        }

        struct WaterOut
        {
            float4 position : SV_Position;
            float2 uv : TEXCOORD0;
        };

        WaterOut waterVertex(float3 p : POSITION)
        {
            WaterOut o;
            o.position = mul(float4(p, 1.0), viewProjection);
            o.uv = p.xy / params.x;
            return o;
        }

        float4 waterPixel(WaterOut i) : SV_Target
        {
            float3 color = flags.x != 0 ? colorTexture.Sample(colorSampler, i.uv).rgb : float3(0.25, 0.4, 0.6);
            color *= water.rgb * (terrainAmbient.rgb + terrainDiffuse.rgb);
            return float4(color, water.a);
        }

        float4 lineVertex(float3 p : POSITION) : SV_Position
        {
            return mul(float4(p, 1.0), viewProjection);
        }

        // Lines already in clip space (the rubber band's outline).
        float4 screenLineVertex(float3 p : POSITION) : SV_Position
        {
            return float4(p.xy, 0.0, 1.0);
        }

        float4 linePixel() : SV_Target
        {
            return baseColor;
        }

        // A rectangle in view space (`rect`, clip space) with a texture (labels, premultiplied) or a color.
        struct LabelOut
        {
            float4 position : SV_Position;
            float2 uv : TEXCOORD0;
        };

        LabelOut labelVertex(uint id : SV_VertexID)
        {
            float2 uv = float2(id & 1, id >> 1);
            LabelOut o;
            o.position = float4(lerp(rect.x, rect.z, uv.x), lerp(rect.y, rect.w, uv.y), 0, 1);
            o.uv = uv;
            return o;
        }

        float4 labelPixel(LabelOut i) : SV_Target
        {
            return colorTexture.Sample(colorSampler, i.uv);
        }

        float4 fillPixel(LabelOut i) : SV_Target
        {
            return baseColor;
        }
        """;

    public const string Preview = """
        cbuffer Uniforms : register(b0)
        {
            row_major float4x4 viewProjection;
            float4 lightDirection;
            float4 baseColor;
            uint4 flags;  // x: textured, y: alpha test, z: lit, w: player color (1 by texture alpha, 2 tint)
            float4 houseColor;
            uint4 bones;  // x: the first bone matrix of the frame
        };

        StructuredBuffer<float4x4> pose : register(t1);  // column-major, as the core writes them
        Texture2D colorTexture : register(t0);
        SamplerState colorSampler : register(s0);

        struct VertexIn
        {
            float3 position : POSITION;
            float3 normal : NORMAL;
            float2 uv : TEXCOORD0;
            uint bone : BLENDINDICES;
        };

        struct VertexOut
        {
            float4 position : SV_Position;
            float3 normal : NORMAL;
            float2 uv : TEXCOORD0;
        };

        VertexOut previewVertex(VertexIn i)
        {
            float4x4 bone = pose[bones.x + i.bone];
            VertexOut o;
            o.position = mul(mul(bone, float4(i.position, 1.0)), viewProjection);
            o.normal = mul(bone, float4(i.normal, 0.0)).xyz;
            o.uv = i.uv;
            return o;
        }

        float4 previewPixel(VertexOut i) : SV_Target
        {
            float4 color = baseColor;
            if (flags.x != 0)
                color = colorTexture.Sample(colorSampler, i.uv);
            // In unit textures alpha marks the team color area, so it only cuts holes when the
            // mesh's shader asks for alpha testing.
            if (flags.w == 1)
                color = float4(lerp(houseColor.rgb, color.rgb, color.a), 1.0);
            else if (flags.w == 2)
                color.rgb *= houseColor.rgb;
            if (flags.y != 0 && color.a < 0.375)
                discard;
            float light = 1.0;
            if (flags.z != 0)
                light = 0.45 + 0.55 * abs(dot(normalize(i.normal), normalize(lightDirection.xyz)));
            return float4(color.rgb * light, color.a);
        }
        """;
}
