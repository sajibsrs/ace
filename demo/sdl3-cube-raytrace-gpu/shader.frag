#version 450

// The sdl3-cube-raytrace scene traced once per pixel, for Vulkan. shader.metal
// is the same program for Metal; shader.sh turns both into shader.ace.

// SDL binds a fragment shader's uniform slot 0 here.
layout(set = 3, binding = 0) uniform Scene {
    vec4 spin;  // sine, cosine of the y-axis angle, then of the x-axis angle
    vec4 light; // unit direction toward the light
    vec4 view;  // output center x, y in pixels, then pixels per unit
} scene;

layout(location = 0) out vec4 o_color;

// Colors run 0 to 255 a channel until the last line. Face 2 * axis + side,
// where side 1 is the positive face.
const vec3 FACE_COLORS[6] = vec3[6](
    vec3(0xF4, 0x43, 0x36), vec3(0x4C, 0xAF, 0x50),
    vec3(0x00, 0xBC, 0xD4), vec3(0x9C, 0x27, 0xB0),
    vec3(0x21, 0x96, 0xF3), vec3(0xFF, 0xA0, 0x00)
);
const vec3 BACKGROUND = vec3(0x10, 0x10, 0x18);
const vec3 FLOOR_COLOR = vec3(0x54, 0x6E, 0x7A);

const float DISTANCE = 4.0;
const float FLOOR_Y = -2.0;
const float AMBIENT = 0.25;
const float EPSILON = 0.001;
const float FAR = 1.0e30;

// A direction in the cube's frame, turned into the camera's.
vec3 to_camera(vec3 v) {
    float z = v.z * scene.spin.y - v.x * scene.spin.x;
    return vec3(v.x * scene.spin.y + v.z * scene.spin.x,
                v.y * scene.spin.w - z * scene.spin.z,
                v.y * scene.spin.z + z * scene.spin.w);
}

// A direction in the camera's frame, turned into the cube's.
vec3 to_cube(vec3 v) {
    float z = v.z * scene.spin.w - v.y * scene.spin.z;
    return vec3(v.x * scene.spin.y - z * scene.spin.x,
                v.y * scene.spin.w + v.z * scene.spin.z,
                v.x * scene.spin.x + z * scene.spin.y);
}

// The face a ray from camera-space origin o along d enters the cube through,
// or -1 on a miss: a slab test in the cube's frame.
int hit(vec3 o, vec3 d) {
    vec3 lo = to_cube(o - vec3(0.0, 0.0, DISTANCE));
    vec3 ld = to_cube(d);
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
vec3 shade(vec3 base, float cosine) {
    return base * (AMBIENT + (1.0 - AMBIENT) * max(cosine, 0.0));
}

void main() {
    vec3 d = vec3((gl_FragCoord.x - scene.view.x) / scene.view.z,
                  (scene.view.y - gl_FragCoord.y) / scene.view.z,
                  1.0);
    vec3 light = scene.light.xyz;
    vec3 color = BACKGROUND;
    int face = hit(vec3(0.0), d);
    if (face >= 0) {
        vec3 normal = vec3(0.0);
        normal[face / 2] = float(face % 2) * 2.0 - 1.0;
        color = shade(FACE_COLORS[face], dot(to_camera(normal), light));
    } else if (d.y < 0.0) {
        // The floor faces straight up; a ray from it toward the light that
        // meets the cube shadows it.
        float t = FLOOR_Y / d.y;
        float lit = hit(d * t, light) >= 0 ? 0.0 : light.y;
        color = shade(FLOOR_COLOR, lit);
    }
    o_color = vec4(color / 255.0, 1.0);
}
