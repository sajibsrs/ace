#include <metal_stdlib>

using namespace metal;

// The sdl3-cube-raytrace scene traced once per pixel, for Metal: shader.frag,
// statement for statement. SDL calls a Metal shader's entry point main0.

struct Scene {
    float4 spin;  // sine, cosine of the y-axis angle, then of the x-axis angle
    float4 light; // unit direction toward the light
    float4 view;  // output center x, y in pixels, then pixels per unit
};

struct Fragment {
    float4 position [[position]];
};

// Colors run 0 to 255 a channel until the last line. Face 2 * axis + side,
// where side 1 is the positive face.
constant float3 FACE_COLORS[6] = {
    float3(0xF4, 0x43, 0x36), float3(0x4C, 0xAF, 0x50),
    float3(0x00, 0xBC, 0xD4), float3(0x9C, 0x27, 0xB0),
    float3(0x21, 0x96, 0xF3), float3(0xFF, 0xA0, 0x00)
};
constant float3 BACKGROUND = float3(0x10, 0x10, 0x18);
constant float3 FLOOR_COLOR = float3(0x54, 0x6E, 0x7A);

constant float DISTANCE = 4.0;
constant float FLOOR_Y = -2.0;
constant float AMBIENT = 0.25;
constant float EPSILON = 0.001;
constant float FAR = 1.0e30;

// A direction in the cube's frame, turned into the camera's.
static float3 to_camera(float4 spin, float3 v) {
    float z = v.z * spin.y - v.x * spin.x;
    return float3(v.x * spin.y + v.z * spin.x,
                  v.y * spin.w - z * spin.z,
                  v.y * spin.z + z * spin.w);
}

// A direction in the camera's frame, turned into the cube's.
static float3 to_cube(float4 spin, float3 v) {
    float z = v.z * spin.w - v.y * spin.z;
    return float3(v.x * spin.y - z * spin.x,
                  v.y * spin.w + v.z * spin.z,
                  v.x * spin.x + z * spin.y);
}

// The face a ray from camera-space origin o along d enters the cube through,
// or -1 on a miss: a slab test in the cube's frame.
static int hit(float4 spin, float3 o, float3 d) {
    float3 lo = to_cube(spin, o - float3(0.0, 0.0, DISTANCE));
    float3 ld = to_cube(spin, d);
    float near = 0.0;
    float far = FAR;
    int face = -1;
    for (int axis = 0; axis < 3; axis++) {
        if (ld[axis] == 0.0) {
            if (lo[axis] < -1.0 || lo[axis] > 1.0) return -1;
            continue;
        }
        float t0 = (-1.0 - lo[axis]) / ld[axis];
        float t1 = (1.0 - lo[axis]) / ld[axis];
        int side = 0;
        if (t0 > t1) {
            float t = t0;
            t0 = t1;
            t1 = t;
            side = 1;
        }
        if (t0 > near) {
            near = t0;
            face = axis * 2 + side;
        }
        if (t1 < far) far = t1;
        if (near > far) return -1;
    }
    if (near < EPSILON) return -1;
    return face;
}

// A base color under Lambert light, which a shadow removes.
static float3 shade(float3 base, float cosine) {
    return base * (AMBIENT + (1.0 - AMBIENT) * max(cosine, 0.0));
}

// SDL binds a fragment shader's uniform slot 0 at buffer 0.
fragment float4 main0(Fragment in [[stage_in]], constant Scene& scene [[buffer(0)]]) {
    float3 d = float3((in.position.x - scene.view.x) / scene.view.z,
                      (scene.view.y - in.position.y) / scene.view.z,
                      1.0);
    float3 light = scene.light.xyz;
    float3 color = BACKGROUND;
    int face = hit(scene.spin, float3(0.0), d);
    if (face >= 0) {
        float3 normal = float3(0.0);
        normal[face / 2] = float(face % 2) * 2.0 - 1.0;
        color = shade(FACE_COLORS[face], dot(to_camera(scene.spin, normal), light));
    } else if (d.y < 0.0) {
        // The floor faces straight up; a ray from it toward the light that
        // meets the cube shadows it.
        float t = FLOOR_Y / d.y;
        float lit = hit(scene.spin, d * t, light) >= 0 ? 0.0 : light.y;
        color = shade(FLOOR_COLOR, lit);
    }
    return float4(color / 255.0, 1.0);
}
